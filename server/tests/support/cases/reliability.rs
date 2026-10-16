use super::super::*;

async fn shutdown_case(tls: bool) -> Result<()> {
    let mut relay = Relay::new_tls("shutdown", tls).await?;
    let mut socket = relay.api.ws(-1, None).await?;
    sync(&mut socket).await?;
    relay
        .api
        .deny("/api/shutdown", Some(json!({})), Some("wrong-token"), 401)
        .await?;
    assert_eq!(
        relay.api.post("/api/shutdown", json!({})).await?,
        json!({"ok":true})
    );
    closed(&mut socket).await?;
    relay.wait_exit().await?;
    assert_eq!(
        std::fs::read_to_string(relay.dir.path().join("adapter-stopped"))?,
        "stdin-closed"
    );
    assert_eq!(
        std::fs::read(relay.dir.path().join("server-stderr.log"))?,
        b""
    );
    Ok(())
}
pub async fn local_shutdown_closes_websockets_and_reaps_adapter() -> Result<()> {
    shutdown_case(false).await
}
pub async fn tls_shutdown_closes_websocket_and_adapter() -> Result<()> {
    shutdown_case(true).await
}
pub async fn different_configs_cannot_share_data_and_lock_releases_after_exit() -> Result<()> {
    let mut relay = Relay::new("demo").await?;
    let path = relay.dir.path().join("second-server.json");
    std::fs::write(&path,json!({"listen":"127.0.0.1:0","token":relay.api.token,"data":relay.dir.path().join("state/.")}).to_string())?;
    let mut command = child_command(binary());
    command.arg("--config").arg(path).arg("--demo");
    let result = run_output(command).await?;
    assert!(!result.status.success());
    assert!(String::from_utf8(result.stderr)?.contains("独占锁"));
    assert!(result.stdout.is_empty());
    assert_eq!(relay.api.get("/api/status").await?["ready"], true);
    let before = relay.api.get("/api/groups").await?;
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(relay.api.get("/api/groups").await?, before);
    Ok(())
}
pub async fn invalid_tls_and_occupied_port_do_not_open_database_or_start_adapter() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let mut make = child_command(binary());
    make.arg("--make-cert").current_dir(relay.dir.path());
    assert!(run_output(make).await?.status.success());
    let cert = relay.dir.path().join(".state/tls/cert.pem");
    let key = cert.with_file_name("key.pem");
    let cases = [
        (json!({"tls_cert":cert}), "同时配置"),
        (json!({"tls_key":key}), "同时配置"),
        (
            json!({"tls_cert":relay.dir.path().join("missing.pem"),"tls_key":key}),
            "TLS 证书或私钥无效",
        ),
        (json!({}), "无法监听"),
        (json!({"tls_cert":cert,"tls_key":key}), "无法监听"),
    ];
    let listen = std::fs::read_to_string(&relay.config)?.parse::<Value>()?["listen"].clone();
    for (index, (tls, error)) in cases.into_iter().enumerate() {
        let data = relay.dir.path().join(format!("invalid-start-{index}"));
        let sentinel = relay.dir.path().join("adapter-must-not-start");
        let mut config = json!({"listen":listen,"token":relay.api.token,"data":data,"bridge_program":std::env::current_exe()?,"bridge_args":["--adapter","startup-sentinel",sentinel]});
        config
            .as_object_mut()
            .unwrap()
            .extend(tls.as_object().unwrap().clone());
        let path = relay.dir.path().join(format!("invalid-{index}.json"));
        std::fs::write(&path, config.to_string())?;
        let mut command = child_command(binary());
        command.arg("--config").arg(path);
        let result = run_output(command).await?;
        assert!(!result.status.success());
        assert!(String::from_utf8(result.stderr)?.contains(error));
        assert!(result.stdout.is_empty());
        assert!(!sentinel.exists());
        assert!(!data.join("relay.sqlite").exists());
    }
    assert_eq!(relay.api.get("/api/status").await?["ready"], true);
    Ok(())
}
pub async fn new_clients_skip_old_events_and_future_cursor_resets() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let mut socket = relay.api.ws(-1, None).await?;
    let initial = sync(&mut socket).await?;
    assert!(!initial.iter().any(|e| e["type"] == "message"));
    let head = initial.last().unwrap()["seq"].as_i64().unwrap();
    assert!(head > 0);
    let value = message("demo-design", "游标测试");
    relay.api.post("/api/messages", value.clone()).await?;
    assert!(
        sent(&mut socket, value["id"].as_str().unwrap())
            .await?
            .last()
            .unwrap()["seq"]
            .as_i64()
            .unwrap()
            > head
    );
    let mut future = relay.api.ws(head + 100000, None).await?;
    let events = sync(&mut future).await?;
    assert_eq!(events[0], json!({"type":"reset","seq":0}));
    assert!(events.last().unwrap()["seq"].as_i64().unwrap() > head);
    assert!(!events.iter().any(|e| e["type"] == "message"));
    Ok(())
}
pub async fn parallel_same_id_generates_one_queue_and_confirmation_event() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let value = message("demo-design", "幂等并发");
    let mut socket = relay.api.ws(-1, None).await?;
    let cursor = sync(&mut socket).await?.last().unwrap()["seq"]
        .as_i64()
        .unwrap();
    let mut jobs = tokio::task::JoinSet::new();
    for _ in 0..16 {
        let api = relay.api.clone();
        let value = value.clone();
        jobs.spawn(async move { api.post("/api/messages", value).await });
    }
    let mut sequences = BTreeSet::new();
    while let Some(result) = jobs.join_next().await {
        sequences.insert(result??["seq"].as_i64().unwrap());
    }
    assert_eq!(sequences.len(), 1);
    sent(&mut socket, value["id"].as_str().unwrap()).await?;
    let mut replay = relay.api.ws(cursor, None).await?;
    let events = sync(&mut replay).await?;
    assert_eq!(
        events
            .iter()
            .filter(|e| e["type"] == "message" && e["message"]["id"] == value["id"])
            .map(|e| e["message"]["status"].as_str().unwrap())
            .collect::<Vec<_>>(),
        ["queued", "sent"]
    );
    Ok(())
}
pub async fn search_matches_visible_text_sender_and_filename_only() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let media = relay
        .api
        .upload("demo-design", "今日设计.gif", b"GIF89atest".to_vec(), None)
        .await?;
    let mut value = message("demo-design", "中文正文");
    value["sender"] = json!("小林");
    value["media"] = json!([media["id"]]);
    relay.api.post("/api/messages", value.clone()).await?;
    let groups = relay.api.get("/api/groups").await?;
    let design = groups
        .as_array()
        .unwrap()
        .iter()
        .find(|g| g["id"] == "demo-design")
        .unwrap();
    assert_eq!(design["preview"], "小林: 中文正文");
    assert!(design["timestamp"].as_i64().unwrap() > 0);
    let unsynced = groups
        .as_array()
        .unwrap()
        .iter()
        .find(|g| g["id"] == "demo-dev")
        .unwrap();
    assert_eq!(unsynced["preview"], "");
    assert_eq!(unsynced["timestamp"], 0);
    for term in ["中文正文", "小林", "设计.gif"] {
        let rows = relay
            .api
            .get(&query(
                "/api/messages",
                &[("group", "demo-design".into()), ("search", term.into())],
            ))
            .await?;
        assert!(ids(&rows).contains(value["id"].as_str().unwrap()));
    }
    for term in [
        value["id"].as_str().unwrap(),
        media["id"].as_str().unwrap(),
        media["sha256"].as_str().unwrap(),
        "fixture-client",
        "timestamp",
    ] {
        let rows = relay
            .api
            .get(&query(
                "/api/messages",
                &[("group", "demo-design".into()), ("search", term.into())],
            ))
            .await?;
        assert!(!ids(&rows).contains(value["id"].as_str().unwrap()));
    }
    Ok(())
}
pub async fn concurrent_messages_reach_both_clients_in_durable_order() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let messages = (0..24)
        .map(|n| message("demo-design", &format!("并发消息 {n}")))
        .collect::<Vec<_>>();
    let ids = messages
        .iter()
        .map(|m| m["id"].as_str().unwrap().to_owned())
        .collect::<BTreeSet<_>>();
    let mut a = relay.api.ws(-1, None).await?;
    let mut b = relay.api.ws(-1, None).await?;
    sync(&mut a).await?;
    sync(&mut b).await?;
    let mut jobs = tokio::task::JoinSet::new();
    for value in messages {
        let api = relay.api.clone();
        jobs.spawn(async move { api.post("/api/messages", value).await });
    }
    while let Some(result) = jobs.join_next().await {
        result??;
    }
    let mut received = Vec::new();
    for socket in [&mut a, &mut b] {
        let mut events = Vec::new();
        let mut confirmed = BTreeSet::new();
        while confirmed != ids {
            let event = recv(socket).await?;
            if event["type"] == "message"
                && ids.contains(event["message"]["id"].as_str().unwrap_or(""))
            {
                if event["message"]["status"] == "sent" {
                    confirmed.insert(event["message"]["id"].as_str().unwrap().to_owned());
                }
                events.push(event);
            }
        }
        assert_eq!(events.len(), 48);
        assert!(
            events
                .windows(2)
                .all(|w| w[0]["seq"].as_i64() < w[1]["seq"].as_i64())
        );
        received.push(events);
    }
    assert_eq!(received[0], received[1]);
    Ok(())
}

pub async fn unconfirmed_send_restart_is_uncertain_and_not_retried() -> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let original = message("fixture-a", "hold without confirmation");
    let id = original["id"].as_str().unwrap();
    relay.api.post("/api/messages", original.clone()).await?;
    relay.state(id, "sending").await?;
    relay.stop().await?;
    assert_eq!(relay.stored(id)?.unwrap()["status"], "uncertain");
    relay.start().await?;
    assert_eq!(
        relay.api.post("/api/messages", original.clone()).await?["status"],
        "uncertain"
    );
    relay
        .confirmed("readiness barrier after reconnect", json!({}))
        .await?;
    assert_eq!(
        relay
            .commands("send")?
            .iter()
            .filter(|c| c["message"]["id"] == id)
            .count(),
        1
    );
    Ok(())
}
