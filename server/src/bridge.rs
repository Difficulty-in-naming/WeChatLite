use crate::{
    App,
    model::{ChatMessage, Group, Member},
};
use anyhow::{Context, Result};
use futures_util::StreamExt;
use serde_json::{Value, json};
use std::{sync::Arc, time::Duration};
use tokio::{
    io::{AsyncRead, AsyncWrite, AsyncWriteExt},
    process::Command,
    sync::mpsc,
    task::JoinHandle,
};
use tokio_util::codec::{FramedRead, LinesCodec};

pub async fn start(app: Arc<App>, mut receiver: mpsc::Receiver<Value>) -> Result<JoinHandle<()>> {
    if app.demo {
        demo_seed(&app).await?;
        *app.forwarding.write().await = app.db(|s| s.forward_groups()).await?.unwrap_or_default();
        app.set_status_identity(true, "演示模式 · 未连接真实微信", "me")
            .await;
        return Ok(tokio::spawn(async move {
            let mut permissions = app.clients_changed.subscribe();
            loop {
                if *app.shutdown.borrow() {
                    return;
                }
                while let Ok(Some(mut message)) = app.db(|s| s.next_queued()).await {
                    let selected = app.forwarding.read().await;
                    let clients = app.clients.read().await;
                    if selected.contains(&message.group)
                        && crate::access::can_submit(
                            &clients,
                            &message.access_client,
                            &message.group,
                        )
                    {
                        message.status = "sent".into();
                        message.source_id = format!("demo:{}", message.id);
                        message.source_ids = vec![message.source_id.clone()];
                    } else {
                        message.status = "failed".into();
                        message.error = "群或客户端发送权限已取消，未提交微信".into();
                    }
                    let event = app.db(move |s| s.update_message(&message)).await;
                    if let Ok(event) = event {
                        app.publish(event);
                    }
                }
                if let Ok(messages) = app.db(|s| s.pending_recalls()).await {
                    for message in messages {
                        let selected = app.forwarding.read().await;
                        let clients = app.clients.read().await;
                        let status = if selected.contains(&message.group)
                            && crate::access::can_submit(
                                &clients,
                                &message.recall_client,
                                &message.group,
                            )
                            && crate::store::check_recall(
                                &message,
                                chrono::Utc::now().timestamp_millis(),
                            )
                            .is_ok()
                        {
                            "recalled"
                        } else {
                            "failed"
                        };
                        if let Ok(events) = app
                            .db(move |s| {
                                s.finish_recall(
                                    &message.id,
                                    &message.recall_attempt,
                                    status,
                                    if status == "failed" {
                                        "超过撤回时限，演示撤回未执行"
                                    } else {
                                        ""
                                    },
                                    status == "failed",
                                )
                            })
                            .await
                        {
                            for event in events {
                                app.publish(event);
                            }
                        }
                    }
                }
                tokio::select! {
                    _ = crate::wait_for_shutdown(app.shutdown.subscribe()) => return,
                    _ = permissions.changed() => {},
                    command = receiver.recv() => {
                        let Some(command) = command else { return; };
                        if command["op"] == "activate" {
                            app.publish(json!({"type":"members","group":command["group"]}));
                        }
                    }
                }
            }
        }));
    }
    if app.config.bridge_program.is_empty() {
        let text = if cfg!(target_os = "macos") {
            "中转服务已启动；Mac 本机微信适配器尚未实现，暂未连接微信"
        } else {
            "请在服务配置中设置 Windows 本机微信适配器"
        };
        app.set_status(false, text).await;
        return Ok(tokio::spawn(crate::wait_for_shutdown(
            app.shutdown.subscribe(),
        )));
    }
    Ok(tokio::spawn(async move {
        let delays = [2, 5, 10, 30];
        let mut attempt = 0;
        loop {
            if *app.shutdown.borrow() || receiver.is_closed() {
                return;
            }
            // run_process reaps the previous child before another adapter may be started.
            let result = run_process(&app, &mut receiver).await;
            if *app.shutdown.borrow() || receiver.is_closed() {
                return;
            }
            if app.status.read().await["ready"] == true {
                attempt = 0;
            }
            let delay = Duration::from_secs(delays[attempt]);
            let reason = result
                .err()
                .map(|error| error.to_string())
                .unwrap_or_else(|| "适配器已退出".into());
            app.set_status(
                false,
                &format!("本机微信未连接：{reason}；{} 秒后自动重试", delay.as_secs()),
            )
            .await;
            tokio::select! {
                _ = crate::wait_for_shutdown(app.shutdown.subscribe()) => return,
                _ = tokio::time::sleep(delay) => {},
            }
            attempt = (attempt + 1).min(delays.len() - 1);
        }
    }))
}

async fn run_process(app: &Arc<App>, receiver: &mut mpsc::Receiver<Value>) -> Result<()> {
    let mut command = Command::new(&app.config.bridge_program);
    command
        .args(&app.config.bridge_args)
        .env("WECHAT_LITE_DATA", &app.config.data)
        .env(
            "WECHAT_LITE_FORWARD_GROUPS",
            serde_json::to_string(&*app.forwarding.read().await)?,
        )
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::inherit())
        .kill_on_drop(true);
    #[cfg(windows)]
    {
        // DETACHED_PROCESS avoids a headless conhost while preserving redirected stdio.
        command.creation_flags(0x00000008);
    }
    let mut child = command.spawn().context("无法启动本机适配器")?;
    let mut input = child.stdin.take().context("适配器没有标准输入")?;
    let mut output = child.stdout.take().context("适配器没有标准输出")?;
    let result = run_stream(app, receiver, &mut input, &mut output).await;
    // EOF lets the adapter close its journal and native helper before a forced stop.
    drop(input);
    // Keep stdout open and drained while the adapter finishes queued commands and cleanup.
    let mut drain = tokio::spawn(async move {
        let _ = tokio::io::copy(&mut output, &mut tokio::io::sink()).await;
    });
    if tokio::time::timeout(Duration::from_secs(3), child.wait())
        .await
        .is_err()
    {
        let _ = child.kill().await;
        let _ = child.wait().await;
    }
    if tokio::time::timeout(Duration::from_secs(1), &mut drain)
        .await
        .is_err()
    {
        drain.abort();
        let _ = drain.await;
    }
    result
}

async fn write_command<W: AsyncWrite + Unpin>(
    app: &App,
    input: &mut W,
    command: Value,
) -> Result<()> {
    let line = format!("{command}\n");
    tokio::select! {
        _ = crate::wait_for_shutdown(app.shutdown.subscribe()) => anyhow::bail!("服务正在退出"),
        result = tokio::time::timeout(Duration::from_secs(8), async {
            input.write_all(line.as_bytes()).await?;
            input.flush().await
        }) => result.context("适配器未及时读取命令")?.map_err(anyhow::Error::from),
    }
}

#[cfg(test)]
async fn import_message(app: &App, message: ChatMessage) -> Result<bool> {
    import_message_kind(app, message, false).await
}

async fn import_message_kind(app: &App, message: ChatMessage, historical: bool) -> Result<bool> {
    import_message_with_quote_source(app, message, historical, None).await
}

async fn import_message_with_quote_source(
    app: &App,
    mut message: ChatMessage,
    historical: bool,
    source_server_id: Option<String>,
) -> Result<bool> {
    if !app.allows(&message.group).await {
        return Ok(false);
    }
    let group = message.group.clone();
    let source = message.source_id.clone();
    if source.is_empty() {
        anyhow::bail!("微信消息缺少稳定来源编号");
    }
    if let Some(existing) = app.db(move |s| s.source(&group, &source)).await? {
        // Refresh native quote metadata retained by older adapter versions, keeping client IDs and recall redaction.
        if existing.status != "recalled"
            && existing.recall_status.is_empty()
            && existing.client_id.is_empty()
            && message.quote.is_some()
        {
            let selected = app.forwarding.read().await;
            if !selected.contains(&existing.group) {
                return Ok(false);
            }
            let text = message.text.clone();
            let quote = message.quote.clone().unwrap();
            if let Some(event) = app
                .db(move |s| {
                    s.enrich_native_quote(
                        &existing.id,
                        &text,
                        &quote,
                        historical,
                        source_server_id.as_deref(),
                    )
                })
                .await?
            {
                app.publish(event);
            }
        }
        return Ok(app.allows(&message.group).await);
    }
    let mut media = Vec::new();
    let imported: Result<()> = async {
        for attachment in &message.media {
            media.push(
                crate::web::import_media(
                    app,
                    &message.group,
                    &attachment.path,
                    &attachment.name,
                    &attachment.kind,
                )
                .await?,
            );
        }
        message.media = media.clone();
        message.status = "received".into();
        let selected = app.forwarding.read().await;
        if !selected.contains(&message.group) {
            anyhow::bail!("该群已取消转发");
        }
        if let Some(event) = app
            .db(move |s| {
                if historical {
                    s.insert_historical_message(&mut message)
                } else {
                    s.insert_message(&mut message)
                }
            })
            .await?
        {
            app.publish(event);
        }
        Ok(())
    }
    .await;
    if imported.is_err() {
        for attachment in media {
            let id = attachment.id;
            if app.db(move |s| s.remove_media(&id)).await.is_ok() {
                let _ = tokio::fs::remove_file(&attachment.path).await;
            }
        }
    }
    imported.map(|()| true)
}

fn valid_source_id(id: &str) -> bool {
    !id.trim().is_empty() && id.len() <= 512 && !id.chars().any(char::is_control)
}

fn apply_ack(message: &mut ChatMessage, value: &Value, now: i64) {
    let status = value["status"].as_str().unwrap_or("uncertain");
    message.status = if ["sent", "failed", "uncertain"].contains(&status) {
        status
    } else {
        "uncertain"
    }
    .into();
    message.error = value["error"].as_str().unwrap_or("").to_owned();
    message.source_id = value["source_id"]
        .as_str()
        .filter(|id| valid_source_id(id))
        .unwrap_or("")
        .to_owned();
    message.source_ids = value["source_ids"]
        .as_array()
        .map(|ids| {
            ids.iter()
                .filter_map(Value::as_str)
                .filter(|id| valid_source_id(id))
                .take(64)
                .map(str::to_owned)
                .collect()
        })
        .unwrap_or_default();
    if message.source_id.is_empty() {
        message.source_id = message.source_ids.first().cloned().unwrap_or_default();
    }
    if message.source_ids.is_empty() && !message.source_id.is_empty() {
        message.source_ids.push(message.source_id.clone());
    }
    // Native create_time has second precision. Ignore stale, unconfirmed or malformed times.
    let earliest = message.timestamp / 1_000 * 1_000;
    if message.status == "sent"
        && !message.source_id.is_empty()
        && let Some(timestamp) = value["timestamp"].as_i64()
        && timestamp > 0
        && timestamp >= earliest
        && timestamp <= now.saturating_add(2_000)
    {
        message.timestamp = timestamp.min(now);
    }
}

async fn run_stream<R: AsyncRead + Unpin, W: AsyncWrite + Unpin>(
    app: &Arc<App>,
    receiver: &mut mpsc::Receiver<Value>,
    input: &mut W,
    output: R,
) -> Result<()> {
    let mut output = FramedRead::new(output, LinesCodec::new_with_max_length(2 * 1024 * 1024));
    let mut inflight: Option<ChatMessage> = None;
    let mut ready = false;
    let mut pending = true;
    let mut policy = app.forwarding_changed.subscribe();
    let mut permissions = app.clients_changed.subscribe();
    let mut configure = true;
    let mut recovering = app.db(|s| s.pending_recalls()).await?;
    let mut recalling = std::collections::BTreeSet::new();
    let result:Result<()>=async {
        loop {
            if *app.shutdown.borrow() { return Ok(()); }
            {
            let selected=app.forwarding.read().await;
            if configure || policy.has_changed().unwrap_or(false) {
                policy.borrow_and_update();
                write_command(app,input,json!({"op":"configure","groups":*selected})).await?;
                configure=false;
            }
            if ready && !recovering.is_empty() {
                for message in recovering.drain(..) {
                    let snapshot=message;
                    let Some(message)=app.db({let id=snapshot.id.clone();move|store|store.message(&id)}).await? else {continue;};
                    if message.recall_status!="pending" || message.recall_attempt!=snapshot.recall_attempt {continue;}
                    if recalling.insert((message.id.clone(),message.recall_attempt.clone())) {write_command(app,input,json!({"op":"recall_check","recall_attempt":message.recall_attempt,"message":message})).await?;}
                }
            }
            if ready && inflight.is_none() && pending {
                let clients=app.clients.read().await;
                if let Some(mut message) = app.db(|s| s.next_queued()).await? {
                    if !selected.contains(&message.group) || !crate::access::can_submit(&clients,&message.access_client,&message.group) {
                        message.status="failed".into();message.error="群或客户端发送权限已取消，未提交微信".into();
                        app.publish(app.db(move|s|s.update_message(&message)).await?);
                        continue;
                    }
                    let mut payload = serde_json::to_value(&message)?;
                    if let Some(quote) = &message.quote {
                        let id=quote.id.clone();
                        let source=app.db(move|s|s.message(&id)).await?;
                        let source=source.filter(|source|source.group==message.group && source.status!="recalled" && !source.source_id.is_empty());
                        let Some(source)=source else {
                            message.status="failed".into();message.error="引用原消息未获微信确认或已撤回，未发送".into();
                            app.publish(app.db(move|s|s.update_message(&message)).await?);
                            continue;
                        };
                        payload["_quote_source"]=json!(source.source_id);
                    }
                    message.status = "sending".into();
                    payload["status"]=json!("sending");
                    app.publish(app.db({let message=message.clone();move |s| s.update_message(&message)}).await?);
                    let command = json!({"op":"send","message":payload});
                    inflight = Some(message);
                    write_command(app, input, command).await?;
                } else { pending = false; }
            }
            }
            tokio::select! {
                _=crate::wait_for_shutdown(app.shutdown.subscribe())=>return Ok(()),
                _=policy.changed()=>{configure=true;pending=true;},
                _=permissions.changed()=>{pending=true;},
                line=output.next()=>{
                    let line=line.context("适配器已退出")?.context("适配器消息无效或超过 2 MiB 容量限制")?;
                    let value:Value=serde_json::from_str(&line).context("适配器协议无效")?;
                    match value["type"].as_str().unwrap_or("") {
                        "status"=>{
                            ready=value["ready"].as_bool().unwrap_or(false);
                            pending=true;
                            app.set_status_recall(ready,value["text"].as_str().unwrap_or("微信状态未知"),value["self_id"].as_str().unwrap_or(""),value["recall_supported"].as_bool(),value["recall_reason"].as_str().unwrap_or("")).await;
                        },
                        "groups"=>{
                            let groups:Vec<Group>=serde_json::from_value(value["groups"].clone())?;
                            app.db(move|s|s.set_groups(&groups)).await?;app.publish(json!({"type":"groups"}));
                        },
                        "members"=>{
                            let group=value["group"].as_str().context("缺少群编号")?.to_owned();
                            let selected=app.forwarding.read().await;
                            if !selected.contains(&group) { continue; }
                            let members:Vec<Member>=serde_json::from_value(value["members"].clone())?;
                            app.db({let group=group.clone();move|s|s.set_members(&group,&members)}).await?;
                            app.publish(json!({"type":"members","group":group}));
                        },
                        "history_done"=>{
                            let group=value["group"].as_str().unwrap_or("").to_owned();
                            let selected=app.forwarding.read().await;
                            let mut value=value;
                            if !selected.contains(&group) {value["error"]=json!("该群已取消转发，历史读取已停止");}
                            app.history.finish(app,value).await?;
                        },
                        "announcement"=>{app.announcements.finish(value).await;},
                        "quote_refresh_done"=>{
                            let group=value["group"].as_str().unwrap_or("");
                            if app.allows(group).await {
                                app.publish(json!({"type":"quote_refresh_done","request_id":value["request_id"],"group":group,"source_id":value["source_id"],"ok":value["ok"]==true,"error":if value["ok"]==true{""}else{"引用原消息暂时无法读取"}}));
                            }
                        },
                        "message"=>{
                            let historical=value["historical"]==true;
                            let mut body=value["message"].clone();
                            let selected=app.forwarding.read().await;
                            let group=body["group"].as_str().unwrap_or_default().to_owned();
                            if !selected.contains(&group) {continue;}
                            drop(selected);
                            if body["_quote_original"].is_object() && body["_quote_original"]["group"]==body["group"] {
                                let original:ChatMessage=serde_json::from_value(body["_quote_original"].clone())?;
                                import_message_kind(app,original,historical).await?;
                            }
                            if let Some(source)=body["_quote_source"].as_str() {
                                let source=source.to_owned();let group=group.clone();
                                if let Some(original)=app.db(move|s|s.source(&group,&source)).await? {
                                    body["quote"]["id"]=json!(original.id);
                                    if original.status=="recalled" {body["quote"]["unavailable"]=json!(true);body["quote"]["text"]=json!("");}
                                }
                            }
                            let source_server_id=body["_quote_server_id"].as_str().map(str::to_owned);
                            let message:ChatMessage=serde_json::from_value(body)?;
                            let group=message.group.clone();let source=message.source_id.clone();
                            match import_message_with_quote_source(app,message,historical,source_server_id).await {
                                Ok(true)=>{},
                                Ok(false)=>continue,
                                Err(error)=>{
                                    app.publish(json!({"type":"warning","text":format!("微信消息导入失败，保留来源消息等待重试：{error}"),"group":group,"source_id":source}));
                                    continue;
                                }
                            }
                            write_command(app,input,json!({"op":"receipt","group":group,"source_id":source})).await?;
                        },
                        "ack"=>{
                            if inflight.as_ref().is_some_and(|m|Some(m.id.as_str())==value["id"].as_str()) {
                                let mut message=inflight.take().unwrap();
                                apply_ack(&mut message,&value,chrono::Utc::now().timestamp_millis());
                                app.publish(app.db(move|s|s.update_message(&message)).await?);
                                pending=true;
                            }
                        },
                        "recall_ack"=>{
                            let id=value["id"].as_str().unwrap_or("").to_owned();
                            let attempt=value["recall_attempt"].as_str().unwrap_or("").to_owned();
                            if recalling.remove(&(id.clone(),attempt.clone())) {
                                let status=value["status"].as_str().unwrap_or("uncertain").to_owned();let error=value["error"].as_str().unwrap_or("").to_owned();let definite=value["definite_not_submitted"]==true;
                                for event in app.db(move|s|s.finish_recall(&id,&attempt,&status,&error,definite)).await? {app.publish(event);}
                            }
                        },
                        "warning"=>{
                            if value["group"].is_string() { app.publish(value); }
                            else {
                                eprintln!("适配器诊断：{}",value["text"].as_str().unwrap_or("未知错误"));
                                app.publish(json!({"type":"warning","text":"本机微信适配器有诊断信息，请在服务端查看"}));
                            }
                        },
                        _=>{}
                    }
                },
                command=receiver.recv()=>{
                    let Some(command)=command else{return Ok(())};
                    if command["op"]=="send" {
                        pending=true;
                    } else if command["op"]=="history" {
                        let selected=app.forwarding.read().await;
                        let clients=app.clients.read().await;
                        let group=command["group"].as_str().unwrap_or("");
                        if !ready || !selected.contains(group) || !crate::access::can_read(&clients,command["_access_client"].as_str().unwrap_or(""),group) {
                            let mut error=command;error["error"]=json!("本机微信未连接或历史读取权限已取消，请重试");
                            app.history.finish(app,error).await?;
                        } else {
                            let mut command=command;if let Some(object)=command.as_object_mut(){object.remove("_access_client");}
                            write_command(app,input,command).await?;
                        }
                    } else if command["op"]=="quote_refresh" {
                        let selected=app.forwarding.read().await;
                        let clients=app.clients.read().await;
                        let group=command["group"].as_str().unwrap_or("");
                        if !ready || !selected.contains(group) || !crate::access::can_read(&clients,command["_access_client"].as_str().unwrap_or(""),group) {
                            app.publish(json!({"type":"quote_refresh_done","request_id":command["request_id"],"group":group,"source_id":command["source_id"],"ok":false,"error":"本机微信未连接或历史读取权限已取消"}));
                        } else {
                            let mut command=command;if let Some(object)=command.as_object_mut(){object.remove("_access_client");}
                            write_command(app,input,command).await?;
                        }
                    } else if command["op"]=="announcement" {
                        let selected=app.forwarding.read().await;
                        let clients=app.clients.read().await;
                        let group=command["group"].as_str().unwrap_or("");
                        if !ready || !selected.contains(group) || !crate::access::can_read(&clients,command["_access_client"].as_str().unwrap_or(""),group) {
                            let mut rejected=crate::announcement::unavailable(group,false);
                            rejected["request_id"]=command["request_id"].clone();
                            app.announcements.finish(rejected).await;
                        } else {
                            let mut command=command;if let Some(object)=command.as_object_mut(){object.remove("_access_client");}
                            write_command(app,input,command).await?;
                        }
                    } else if command["op"]=="recall" {
                        let id=command["message"]["id"].as_str().unwrap_or("").to_owned();
                        let attempt=command["message"]["recall_attempt"].as_str().unwrap_or("").to_owned();
                        if recalling.contains(&(id.clone(),attempt.clone())){continue;}
                        let Some(message)=app.db(move|s|s.message(&id)).await? else {continue;};
                        if message.recall_status!="pending" || message.recall_attempt!=attempt {continue;}
                        if message.recall_attempt.is_empty() {
                            if !ready { recovering.push(message); }
                            else if recalling.insert((message.id.clone(),String::new())) {
                                write_command(app,input,json!({"op":"recall_check","recall_attempt":"","message":message})).await?;
                            }
                            continue;
                        }
                        let unavailable={let status=app.status.read().await;crate::model::recall_unavailable(&status).map(str::to_owned)};
                        if let Some(reason)=unavailable {
                            for event in app.db(move|s|s.finish_recall(&message.id,&message.recall_attempt,"failed",&reason,true)).await?{app.publish(event);}
                            continue;
                        }
                        let selected=app.forwarding.read().await;
                        let clients=app.clients.read().await;
                        if !selected.contains(&message.group) || !crate::access::can_submit(&clients,&message.recall_client,&message.group) || crate::store::check_recall(&message,chrono::Utc::now().timestamp_millis()).is_err() {
                            for event in app.db(move|s|s.finish_recall(&message.id,&message.recall_attempt,"failed","群或客户端发送权限已取消，或超过撤回时限，未提交微信",true)).await?{app.publish(event);}
                        } else if !ready {recovering.push(message);}
                        else {recalling.insert((message.id.clone(),message.recall_attempt.clone()));write_command(app,input,json!({"op":"recall","recall_attempt":message.recall_attempt,"message":message,"explicit_retry":command["explicit_retry"]==true})).await?;}
                    } else {
                        let selected=app.forwarding.read().await;
                        let clients=app.clients.read().await;
                        if command["op"]=="activate" && !command["group"].as_str().is_some_and(|group|selected.contains(group) && crate::access::can_read(&clients,command["_access_client"].as_str().unwrap_or(""),group)) { continue; }
                        let mut command=command;
                        if let Some(object)=command.as_object_mut(){object.remove("_access_client");}
                        write_command(app,input,command).await?;
                    }
                }
            }
        }
    }.await;
    app.history
        .fail_all("本机微信连接中断，历史读取未完成，请重试")
        .await;
    app.announcements.fail_all().await;
    if let Some(mut message) = inflight {
        message.status = "uncertain".into();
        message.error = "微信确认前连接中断，请核对微信群，未自动重发".into();
        if let Ok(event) = app.db(move |s| s.update_message(&message)).await {
            app.publish(event);
        }
    }
    result
}

async fn demo_seed(app: &Arc<App>) -> Result<()> {
    app.db(|s|{
        s.set_groups(&[
            Group{id:"demo-design".into(),name:"产品设计讨论组".into(),count:12,avatar:String::new()},
            Group{id:"demo-dev".into(),name:"研发协作群".into(),count:8,avatar:String::new()},
            Group{id:"demo-files".into(),name:"文件传输助手".into(),count:1,avatar:String::new()},
        ])?;
        s.initialize_forward_groups(&std::collections::BTreeSet::from(["demo-design".into(),"demo-dev".into(),"demo-files".into()]))?;
        let members=[("lin","林小满"),("chen","陈序"),("zhou","周可"),("nick","Nick"),("me","我")].into_iter().map(|(id,name)|Member{id:id.into(),name:name.into(),remark:String::new(),avatar:String::new()}).collect::<Vec<_>>();
        for group in ["demo-design","demo-dev","demo-files"]{s.set_members(group,&members)?;}
        let messages=[("welcome-1","林小满","今天的设计稿放这里，我们在这个群里一起确认。",false),
            ("welcome-2","陈序","收到。文件直接拖进输入框就可以附加，确认后再发送。",false),
            ("welcome-3","我","好，我把视频和 GIF 一起整理过来。",true),
            ("welcome-4","周可","输入 @ 就能选择成员，方向键切换，回车确认。",false)];
        for (index,(id,sender,text,outgoing)) in messages.into_iter().enumerate(){
            let mut message:ChatMessage=serde_json::from_value(json!({"id":id,"group":"demo-design","sender":sender,"sender_id":if outgoing{"me"}else{"lin"},"text":text,
                "timestamp":chrono::Utc::now().timestamp_millis()-((4-index)as i64)*90_000,"status":"received","source_id":id,"outgoing":outgoing}))?;
            s.insert_message(&mut message)?;
        }
        Ok(())
    }).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{DuplexStream, ReadHalf, WriteHalf};

    fn message(id: &str) -> ChatMessage {
        serde_json::from_value(json!({"id":id,"group":"g","sender":"测试","text":"你好","timestamp":0,"status":"queued","outgoing":true})).unwrap()
    }
    #[tokio::test]
    async fn native_history_refresh_adds_quote_metadata_without_reviving_recalled_or_client_content()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, _) = crate::test_app(temp.path());
        for (id, status, client, historical) in [
            ("legacy", "received", "", false),
            ("legacy-historical", "received", "", true),
            ("withdrawn", "recalled", "", true),
            ("client-owned", "sent", "client", true),
        ] {
            let mut old = message(id);
            old.status = status.into();
            old.client_id = client.into();
            old.source_id = format!("native:{id}");
            if status == "recalled" {
                old.text.clear();
            }
            app.store.insert_message(&mut old).unwrap();
            let mut fresh = old.clone();
            fresh.id = format!("fresh-{id}");
            fresh.text = "真实引用回复".into();
            fresh.quote = Some(
                serde_json::from_value(
                    json!({"id":"source","sender":"成员","text":"原话","unavailable":false}),
                )
                .unwrap(),
            );
            let head = app.store.event_head().unwrap();
            let mut broadcast = app.events.subscribe();
            assert!(import_message_kind(&app, fresh, historical).await.unwrap());
            let stored = app.store.message(id).unwrap().unwrap();
            if id.starts_with("legacy") {
                assert!(stored.quote.is_some());
                assert_eq!(stored.text, "真实引用回复");
                let event = broadcast.try_recv().unwrap();
                assert_eq!(event["message"]["id"], id);
                assert_eq!(event["historical"].as_bool(), historical.then_some(true));
                let replay = app.store.events(head).unwrap();
                assert_eq!(replay.len(), 1);
                assert_eq!(replay[0]["message"]["id"], id);
                assert_eq!(
                    replay[0]["historical"].as_bool(),
                    historical.then_some(true)
                );
            } else {
                assert!(stored.quote.is_none());
                assert_eq!(stored.text, old.text);
                assert_eq!(stored.status, status);
                assert!(broadcast.try_recv().is_err());
                assert!(app.store.events(head).unwrap().is_empty());
            }
        }
    }
    async fn fixture(
        app: Arc<App>,
        mut receiver: mpsc::Receiver<Value>,
    ) -> (
        JoinHandle<Result<()>>,
        WriteHalf<DuplexStream>,
        FramedRead<ReadHalf<DuplexStream>, LinesCodec>,
    ) {
        let (relay, adapter) = tokio::io::duplex(64 * 1024);
        let (output, mut input) = tokio::io::split(relay);
        let (read, write) = tokio::io::split(adapter);
        let task =
            tokio::spawn(async move { run_stream(&app, &mut receiver, &mut input, output).await });
        let mut input = FramedRead::new(read, LinesCodec::new());
        assert_eq!(
            next(&mut input).await,
            json!({"op":"configure","groups":["g"]})
        );
        (task, write, input)
    }
    async fn emit(output: &mut WriteHalf<DuplexStream>, event: Value) {
        output
            .write_all(format!("{event}\n").as_bytes())
            .await
            .unwrap();
    }
    async fn next(input: &mut FramedRead<ReadHalf<DuplexStream>, LinesCodec>) -> Value {
        let line = tokio::time::timeout(Duration::from_secs(2), input.next())
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        serde_json::from_str(&line).unwrap()
    }

    #[tokio::test]
    async fn historical_adapter_message_repairs_a_legacy_unresolved_quote_with_exact_server_proof()
    {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut original = message("real-image");
        original.status = "received".into();
        original.source_id = "wx:g:message__message_0.db:3:30".into();
        app.store.insert_message(&mut original).unwrap();
        let mut legacy = message("legacy-image-reply");
        legacy.status = "received".into();
        legacy.source_id = "wx:g:message__message_0.db:4:40".into();
        legacy.quote = Some(
            serde_json::from_value(json!({"id":"unavailable-42","sender":"成员","text":"[图片]"}))
                .unwrap(),
        );
        app.store.insert_message(&mut legacy).unwrap();
        let head = app.store.event_head().unwrap();
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        let mut broadcasts = app.events.subscribe();
        emit(&mut output, json!({"type":"status","ready":true})).await;
        recall_barrier(&mut output, &mut broadcasts).await;
        let request = uuid::Uuid::new_v4().to_string();
        let command = json!({"op":"quote_refresh","group":"g","source_id":legacy.source_id,"request_id":request});
        let mut authorized_command = command.clone();
        authorized_command["_access_client"] = json!("");
        app.commands.send(authorized_command).await.unwrap();
        assert_eq!(next(&mut input).await, command);
        let mut fresh = json!(&legacy);
        fresh["id"] = json!("fresh-reply-id");
        fresh["quote"]["id"] = json!("stable-native-image-id");
        fresh["_quote_source"] = json!(&original.source_id);
        fresh["_quote_server_id"] = json!("42");
        emit(
            &mut output,
            json!({"type":"message","historical":true,"message":fresh}),
        )
        .await;
        assert_eq!(
            next(&mut input).await,
            json!({"op":"receipt","group":"g","source_id":legacy.source_id})
        );
        emit(&mut output, json!({"type":"quote_refresh_done","group":"g","source_id":legacy.source_id,"request_id":request,"ok":true})).await;
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let event = broadcasts.recv().await.unwrap();
                if event["type"] == "quote_refresh_done" && event["request_id"] == request {
                    assert_eq!(event["ok"], true);
                    // Completion may only be observed after the corrected link is committed.
                    assert_eq!(
                        app.store
                            .message(&legacy.id)
                            .unwrap()
                            .unwrap()
                            .quote
                            .unwrap()
                            .id,
                        original.id
                    );
                    break;
                }
            }
        })
        .await
        .unwrap();
        let repaired = app.store.message(&legacy.id).unwrap().unwrap();
        assert_eq!(repaired.quote.as_ref().unwrap().id, original.id);
        assert_eq!(repaired.id, legacy.id);
        assert_eq!(repaired.seq, legacy.seq);
        let events = app.store.events(head).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["historical"], true);
        assert_eq!(events[0]["message"]["id"], legacy.id);
        task.abort();
        let _ = task.await;
    }

    #[tokio::test]
    async fn quote_refresh_rechecks_readiness_selection_and_read_access_before_forwarding() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        let mut events = app.events.subscribe();
        for (ready, selected, client) in [
            (false, true, ""),
            (true, true, "missing-client"),
            (true, false, ""),
        ] {
            emit(&mut output, json!({"type":"status","ready":ready})).await;
            recall_barrier(&mut output, &mut events).await;
            if !selected {
                app.set_forwarding(Default::default()).await.unwrap();
                assert_eq!(
                    next(&mut input).await,
                    json!({"op":"configure","groups":[]})
                );
            }
            let request = uuid::Uuid::new_v4().to_string();
            app.commands.send(json!({"op":"quote_refresh","request_id":request,"group":"g","source_id":"wx:g:message__message_0.db:4:40","_access_client":client})).await.unwrap();
            tokio::time::timeout(Duration::from_secs(2), async {
                loop {
                    let event = events.recv().await.unwrap();
                    if event["type"] == "quote_refresh_done" && event["request_id"] == request {
                        assert_eq!(event["ok"], false);
                        break;
                    }
                }
            })
            .await
            .unwrap();
            assert!(
                tokio::time::timeout(Duration::from_millis(40), input.next())
                    .await
                    .is_err()
            );
        }
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    async fn recall_barrier(
        output: &mut WriteHalf<DuplexStream>,
        events: &mut tokio::sync::broadcast::Receiver<Value>,
    ) {
        emit(
            output,
            json!({"type":"warning","group":"g","text":"offline recall barrier"}),
        )
        .await;
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if events.recv().await.unwrap()["text"] == "offline recall barrier" {
                    break;
                }
            }
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn recall_capability_is_forwarded_and_preserved_on_disconnect_but_legacy_status_resets_it()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut events = app.events.subscribe();
        let (task, mut output, _input) = fixture(app.clone(), receiver).await;
        emit(&mut output,json!({"type":"status","ready":true,"recall_supported":false,"recall_reason":"请在原版微信中撤回"})).await;
        let status = events.recv().await.unwrap();
        assert_eq!(status["recall_supported"], false);
        assert_eq!(status["recall_reason"], "请在原版微信中撤回");
        app.set_status(false, "fixture disconnected").await;
        assert_eq!(app.status.read().await["recall_supported"], false);
        emit(&mut output, json!({"type":"status","ready":true})).await;
        recall_barrier(&mut output, &mut events).await;
        assert!(app.status.read().await.get("recall_supported").is_none());
        emit(&mut output,json!({"type":"status","ready":true,"recall_supported":false,"recall_reason":"x".repeat(257)})).await;
        recall_barrier(&mut output, &mut events).await;
        assert_eq!(
            app.status.read().await["recall_reason"],
            crate::model::RECALL_UNAVAILABLE
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn queued_recall_is_never_sent_when_adapter_declares_background_recall_unavailable() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut source = message("unsupported-recall");
        source.status = "sent".into();
        source.source_id = "native-source".into();
        source.timestamp = chrono::Utc::now().timestamp_millis();
        app.store.insert_message(&mut source).unwrap();
        let mut events = app.events.subscribe();
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(
            &mut output,
            json!({"type":"status","ready":true,"recall_supported":false}),
        )
        .await;
        recall_barrier(&mut output, &mut events).await;
        let pending = app
            .store
            .begin_recall(&source.id, source.timestamp)
            .unwrap()
            .0;
        app.commands
            .send(json!({"op":"recall","message":pending}))
            .await
            .unwrap();
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let event = events.recv().await.unwrap();
                if event["message"]["id"] == source.id
                    && event["message"]["recall_status"] == "failed"
                {
                    break;
                }
            }
        })
        .await
        .unwrap();
        assert!(
            tokio::time::timeout(Duration::from_millis(60), input.next())
                .await
                .is_err()
        );
        assert_eq!(
            app.store.message(&source.id).unwrap().unwrap().recall_error,
            crate::model::RECALL_UNAVAILABLE
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn adapter_status_identity_is_forwarded_bounded_and_cleared_on_disconnect_or_missing_id()
    {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut events = app.events.subscribe();
        let (task, mut output, _input) = fixture(app.clone(), receiver).await;
        for (ready, identity, expected) in [
            (true, json!("wxid_fixture_self"), "wxid_fixture_self"),
            (false, json!("wxid_fixture_self"), ""),
            (true, json!("other-real-id"), "other-real-id"),
            (true, Value::Null, ""),
            (true, json!(" "), ""),
            (true, json!("bad\nidentity"), ""),
            (true, json!("notify@all"), ""),
            (true, json!("x".repeat(1025)), ""),
            (true, json!(42), ""),
        ] {
            emit(
                &mut output,
                json!({"type":"status","ready":ready,"self_id":identity}),
            )
            .await;
            let status = tokio::time::timeout(Duration::from_secs(1), events.recv())
                .await
                .unwrap()
                .unwrap();
            assert_eq!(status["self_id"], expected);
            assert_eq!(app.status.read().await["self_id"], expected);
        }
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[test]
    fn confirmed_send_time_validates_status_source_milliseconds_and_clock_tolerance() {
        let queued = 1_700_000_000_789i64;
        let now = queued + 600_000;
        let ack = json!({"status":"sent","source_id":"native-source","timestamp":now-789});
        let mut sent = message("delayed");
        sent.timestamp = queued;
        apply_ack(&mut sent, &ack, now);
        assert_eq!(sent.timestamp, now - 789);
        assert!(crate::store::check_recall(&sent, now).is_ok());

        for timestamp in [
            Value::Null,
            json!(0),
            json!(-1),
            json!(now.to_string()),
            json!(now as f64),
            json!(now / 1000),
            json!(queued / 1000 * 1000 - 1),
            json!(now + 2001),
            json!(u64::MAX),
        ] {
            let mut candidate = message("invalid-time");
            candidate.timestamp = queued;
            let mut invalid = ack.clone();
            invalid["timestamp"] = timestamp;
            apply_ack(&mut candidate, &invalid, now);
            assert_eq!(candidate.timestamp, queued, "invalid ACK time: {invalid}");
        }
        for changes in [
            json!({"status":"failed"}),
            json!({"status":"uncertain"}),
            json!({"status":"unknown"}),
            json!({"source_id":""}),
            json!({"source_id":" "}),
            json!({"source_id":"invalid\nsource"}),
            json!({"source_id":"x".repeat(513)}),
        ] {
            let mut candidate = message("unconfirmed-time");
            candidate.timestamp = queued;
            let mut invalid = ack.clone();
            for (key, value) in changes.as_object().unwrap() {
                invalid[key] = value.clone();
            }
            apply_ack(&mut candidate, &invalid, now);
            assert_eq!(candidate.timestamp, queued, "unconfirmed ACK: {invalid}");
        }
        for (timestamp, expected) in [
            (queued / 1000 * 1000, queued / 1000 * 1000),
            (now + 2000, now),
        ] {
            let mut candidate = message("second-precision");
            candidate.timestamp = queued;
            let mut valid = ack.clone();
            valid["timestamp"] = json!(timestamp);
            apply_ack(&mut candidate, &valid, now);
            assert_eq!(candidate.timestamp, expected);
        }
        let mut fallback = message("multiple-parts");
        fallback.timestamp = queued;
        apply_ack(
            &mut fallback,
            &json!({"status":"sent","source_ids":["","bad\nsource","first-part","second-part"],"timestamp":now}),
            now,
        );
        assert_eq!(fallback.timestamp, now);
        assert_eq!(fallback.source_id, "first-part");
        assert_eq!(fallback.source_ids, vec!["first-part", "second-part"]);
        let mut legacy = message("legacy");
        legacy.timestamp = queued;
        apply_ack(
            &mut legacy,
            &json!({"status":"sent","source_id":"native-source"}),
            now,
        );
        assert_eq!(legacy.timestamp, queued);
    }

    #[tokio::test]
    async fn announcement_roundtrip_strips_internal_access_and_checks_permission_before_native_read()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        tokio::time::timeout(Duration::from_secs(1), async {
            while app.status.read().await["ready"] != true {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        let (id, mut reply) = app.announcements.request(&app, "g", "").await.unwrap();
        let command = next(&mut input).await;
        assert_eq!(command["op"], "announcement");
        assert_eq!(command["request_id"], id);
        assert!(command.get("_access_client").is_none());
        emit(&mut output,json!({"type":"announcement","group":"g","request_id":id,"available":true,"set":true,"text":"离线公告","author_id":"actor","author":"成员","avatar":"","updated_at":1700000000123i64})).await;
        tokio::time::timeout(Duration::from_secs(1), reply.changed())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(reply.borrow().as_ref().unwrap()["text"], "离线公告");
        assert!(
            app.store.events(0).unwrap().is_empty(),
            "announcements must not enter durable chat replay"
        );
        let (_, mut rejected) = app
            .announcements
            .request(&app, "g", "missing-client")
            .await
            .unwrap();
        tokio::time::timeout(Duration::from_secs(1), rejected.changed())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(rejected.borrow().as_ref().unwrap()["available"], false);
        assert!(
            tokio::time::timeout(Duration::from_millis(50), input.next())
                .await
                .is_err(),
            "rejected read reached native adapter"
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn delayed_send_uses_confirmed_time_for_persisted_recall_window() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut delayed = message("delayed-send");
        delayed.timestamp = chrono::Utc::now().timestamp_millis() - 600_000;
        app.store.insert_message(&mut delayed).unwrap();
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        assert_eq!(next(&mut input).await["message"]["id"], delayed.id);
        let confirmed = chrono::Utc::now().timestamp_millis() / 1000 * 1000;
        let mut events = app.events.subscribe();
        emit(&mut output,json!({"type":"ack","id":delayed.id,"status":"sent","source_id":"native-source","timestamp":confirmed})).await;
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let event = events.recv().await.unwrap();
                if event["message"]["id"] == delayed.id && event["message"]["status"] == "sent" {
                    break;
                }
            }
        })
        .await
        .unwrap();
        let stored = app.store.message(&delayed.id).unwrap().unwrap();
        assert_eq!(stored.timestamp, confirmed);
        assert!(
            app.store
                .begin_recall(&delayed.id, chrono::Utc::now().timestamp_millis())
                .unwrap()
                .1
                .is_some()
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn missing_attachment_is_unacknowledged_and_other_messages_continue() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let first = temp.path().join("first.txt");
        let missing = temp.path().join("missing.gif");
        std::fs::write(&first, b"first").unwrap();
        let mut bad = message("bad");
        bad.source_id = "source-bad".into();
        bad.outgoing = false;
        bad.media = vec![
            crate::model::Media {
                id: "a".into(),
                name: "first.txt".into(),
                kind: "file".into(),
                size: 5,
                sha256: String::new(),
                path: first.to_string_lossy().into_owned(),
            },
            crate::model::Media {
                id: "b".into(),
                name: "missing.gif".into(),
                kind: "gif".into(),
                size: 6,
                sha256: String::new(),
                path: missing.to_string_lossy().into_owned(),
            },
        ];
        let mut good = message("good");
        good.source_id = "source-good".into();
        good.outgoing = false;
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"message","message":bad})).await;
        emit(&mut output, json!({"type":"message","message":good})).await;
        let receipt = next(&mut input).await;
        assert_eq!(receipt["source_id"], "source-good");
        assert!(app.store.source("g", "source-bad").unwrap().is_none());
        assert!(app.store.source("g", "source-good").unwrap().is_some());
        for day in std::fs::read_dir(temp.path().join("media")).unwrap() {
            assert_eq!(std::fs::read_dir(day.unwrap().path()).unwrap().count(), 0);
        }
        std::fs::write(&missing, b"GIF89a").unwrap();
        emit(&mut output, json!({"type":"message","message":bad})).await;
        assert_eq!(next(&mut input).await["source_id"], "source-bad");
        let imported = app.store.source("g", "source-bad").unwrap().unwrap();
        assert_eq!(imported.media.len(), 2);
        for media in imported.media {
            assert!(std::path::Path::new(&media.path).exists());
        }
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn readiness_partial_lines_shutdown_and_restart_preserve_the_queue() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        for id in ["first", "second", "third"] {
            app.store.insert_message(&mut message(id)).unwrap();
        }
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":false})).await;
        app.commands.send(json!({"op":"send"})).await.unwrap();
        assert!(
            tokio::time::timeout(Duration::from_millis(40), input.next())
                .await
                .is_err()
        );
        // A command arriving between fragments must not cancel and lose the partial line.
        output.write_all(b"{\"type\":\"sta").await.unwrap();
        app.commands
            .send(json!({"op":"activate","group":"g"}))
            .await
            .unwrap();
        assert_eq!(next(&mut input).await["op"], "activate");
        output.write_all(b"tus\",\"ready\":true}\n").await.unwrap();
        assert_eq!(next(&mut input).await["message"]["id"], "first");
        app.commands.send(json!({"op":"send"})).await.unwrap();
        app.commands.send(json!({"op":"send"})).await.unwrap();
        assert!(
            tokio::time::timeout(Duration::from_millis(40), input.next())
                .await
                .is_err()
        );
        emit(
            &mut output,
            json!({"type":"ack","id":"first","status":"sent"}),
        )
        .await;
        assert_eq!(next(&mut input).await["message"]["id"], "second");
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
        assert_eq!(app.store.message("first").unwrap().unwrap().status, "sent");
        assert_eq!(
            app.store.message("second").unwrap().unwrap().status,
            "uncertain"
        );
        assert_eq!(
            app.store.message("third").unwrap().unwrap().status,
            "queued"
        );
        drop(app);
        let (app, receiver) = crate::test_app(temp.path());
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        assert_eq!(next(&mut input).await["message"]["id"], "third");
        emit(
            &mut output,
            json!({"type":"ack","id":"third","status":"sent"}),
        )
        .await;
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn deselection_cancels_queue_but_only_confirms_already_submitted_messages() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        for id in ["submitted", "queued"] {
            app.store.insert_message(&mut message(id)).unwrap();
        }
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        assert_eq!(next(&mut input).await["message"]["id"], "submitted");
        app.set_forwarding(std::collections::BTreeSet::new())
            .await
            .unwrap();
        assert_eq!(
            next(&mut input).await,
            json!({"op":"configure","groups":[]})
        );
        assert_eq!(
            app.store.message("queued").unwrap().unwrap().status,
            "failed"
        );
        emit(
            &mut output,
            json!({"type":"ack","id":"submitted","status":"sent"}),
        )
        .await;
        assert!(
            tokio::time::timeout(Duration::from_millis(60), input.next())
                .await
                .is_err()
        );
        assert_eq!(
            app.store.message("submitted").unwrap().unwrap().status,
            "sent"
        );
        let mut incoming = message("unselected");
        incoming.source_id = "secret-source".into();
        incoming.media.push(crate::model::Media {
            id: "secret".into(),
            name: "secret.txt".into(),
            kind: "file".into(),
            size: 1,
            sha256: String::new(),
            path: "missing-should-not-be-read".into(),
        });
        assert!(!import_message(&app, incoming).await.unwrap());
        assert!(app.store.message("unselected").unwrap().is_none());
        assert_eq!(
            std::fs::read_dir(temp.path().join("media"))
                .unwrap()
                .count(),
            0
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn pending_recall_recovers_with_a_read_only_check_instead_of_a_new_recall() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut source = message("recover-recall");
        source.status = "sent".into();
        source.source_id = "native-source".into();
        source.timestamp = chrono::Utc::now().timestamp_millis();
        app.store.insert_message(&mut source).unwrap();
        app.store
            .begin_recall(&source.id, source.timestamp)
            .unwrap();
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        let check = next(&mut input).await;
        assert_eq!(check["op"], "recall_check");
        assert_eq!(check["message"]["id"], source.id);
        emit(
            &mut output,
            json!({"type":"recall_ack","id":source.id,"recall_attempt":check["recall_attempt"],"status":"recalled"}),
        )
        .await;
        assert!(
            tokio::time::timeout(Duration::from_millis(60), input.next())
                .await
                .is_err()
        );
        assert_eq!(
            app.store.message(&source.id).unwrap().unwrap().status,
            "recalled"
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn late_old_recall_ack_cannot_finish_or_unlock_a_new_explicit_attempt() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut source = message("nonce-recall");
        source.status = "sent".into();
        source.source_id = "native".into();
        source.timestamp = chrono::Utc::now().timestamp_millis();
        app.store.insert_message(&mut source).unwrap();
        let first = app
            .store
            .begin_recall(&source.id, source.timestamp)
            .unwrap()
            .0;
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        let check = next(&mut input).await;
        assert_eq!(check["op"], "recall_check");
        assert_eq!(check["recall_attempt"], first.recall_attempt);
        app.store
            .finish_recall(
                &source.id,
                &first.recall_attempt,
                "failed",
                "not submitted",
                true,
            )
            .unwrap();
        let second = app
            .store
            .begin_recall(&source.id, source.timestamp + 1)
            .unwrap()
            .0;
        assert_ne!(first.recall_attempt, second.recall_attempt);
        app.commands
            .send(json!({"op":"recall","message":second,"explicit_retry":true}))
            .await
            .unwrap();
        let current = next(&mut input).await;
        assert_eq!(current["op"], "recall");
        assert_eq!(current["recall_attempt"], second.recall_attempt);
        let mut events = app.events.subscribe();
        for ack in [
            json!({"type":"recall_ack","id":source.id,"recall_attempt":first.recall_attempt,"status":"failed","definite_not_submitted":true}),
            json!({"type":"recall_ack","id":source.id,"recall_attempt":first.recall_attempt,"status":"recalled"}),
            json!({"type":"recall_ack","id":source.id,"status":"recalled"}),
        ] {
            emit(&mut output, ack).await;
        }
        recall_barrier(&mut output, &mut events).await;
        let saved = app.store.message(&source.id).unwrap().unwrap();
        assert_eq!(saved.recall_attempt, second.recall_attempt);
        assert_eq!(saved.recall_status, "pending");
        assert!(!saved.recall_retryable);
        assert_eq!(saved.text, source.text);
        emit(&mut output, json!({"type":"recall_ack","id":source.id,"recall_attempt":second.recall_attempt,"status":"recalled"})).await;
        recall_barrier(&mut output, &mut events).await;
        assert_eq!(
            app.store.message(&source.id).unwrap().unwrap().status,
            "recalled"
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn stale_recovery_snapshot_is_skipped_before_a_new_explicit_request_is_dispatched() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut source = message("stale-recovery");
        source.status = "sent".into();
        source.source_id = "native".into();
        source.timestamp = chrono::Utc::now().timestamp_millis();
        app.store.insert_message(&mut source).unwrap();
        let first = app
            .store
            .begin_recall(&source.id, source.timestamp)
            .unwrap()
            .0;
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        app.store
            .finish_recall(
                &source.id,
                &first.recall_attempt,
                "failed",
                "not submitted",
                true,
            )
            .unwrap();
        let second = app
            .store
            .begin_recall(&source.id, source.timestamp + 1)
            .unwrap()
            .0;
        let mut events = app.events.subscribe();
        emit(&mut output, json!({"type":"status","ready":true})).await;
        recall_barrier(&mut output, &mut events).await;
        assert!(
            tokio::time::timeout(Duration::from_millis(30), input.next())
                .await
                .is_err()
        );
        app.commands
            .send(json!({"op":"recall","message":first,"explicit_retry":false}))
            .await
            .unwrap();
        app.commands
            .send(json!({"op":"recall","message":second,"explicit_retry":true}))
            .await
            .unwrap();
        let command = next(&mut input).await;
        assert_eq!(command["op"], "recall");
        assert_eq!(command["recall_attempt"], second.recall_attempt);
        assert_eq!(command["explicit_retry"], true);
        emit(&mut output, json!({"type":"recall_ack","id":source.id,"recall_attempt":second.recall_attempt,"status":"failed","definite_not_submitted":true})).await;
        recall_barrier(&mut output, &mut events).await;
        assert!(
            app.store
                .message(&source.id)
                .unwrap()
                .unwrap()
                .recall_retryable
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }

    #[tokio::test]
    async fn legacy_pending_recovery_is_read_only_and_cannot_gain_retry_proof() {
        let temp = tempfile::tempdir().unwrap();
        let (app, receiver) = crate::test_app(temp.path());
        let mut source = message("legacy-pending");
        source.status = "sent".into();
        source.source_id = "native".into();
        source.timestamp = chrono::Utc::now().timestamp_millis();
        source.recall_status = "pending".into();
        app.store.insert_message(&mut source).unwrap();
        let (task, mut output, mut input) = fixture(app.clone(), receiver).await;
        emit(&mut output, json!({"type":"status","ready":true})).await;
        let command = next(&mut input).await;
        assert_eq!(command["op"], "recall_check");
        assert_eq!(command["recall_attempt"], "");
        let mut events = app.events.subscribe();
        emit(&mut output, json!({"type":"recall_ack","id":source.id,"status":"failed","definite_not_submitted":true})).await;
        recall_barrier(&mut output, &mut events).await;
        let saved = app.store.message(&source.id).unwrap().unwrap();
        assert_eq!(saved.recall_status, "failed");
        assert!(!saved.recall_retryable);
        assert!(
            tokio::time::timeout(Duration::from_millis(30), input.next())
                .await
                .is_err()
        );
        app.shutdown.send_replace(true);
        task.await.unwrap().unwrap();
    }
}
