use anyhow::{Context, Result, ensure};
use rusqlite::{Connection, params};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeSet, HashMap},
    path::{Path, PathBuf},
};

pub fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value[key].as_str().unwrap_or_default()
}

pub fn source(group: &str, row: &Value) -> Result<String> {
    Ok(format!(
        "wx:{group}:{}:{}:{}",
        row["shard"].as_str().context("消息缺少分片")?,
        row["local_id"].as_i64().context("消息缺少本地编号")?,
        row["sort_seq"].as_i64().context("消息缺少游标")?
    ))
}

pub fn xml(value: &str) -> Result<roxmltree::Document<'_>> {
    ensure!(
        value.len() <= 2 * 1024 * 1024
            && !value.contains("<!DOCTYPE")
            && !value.contains("<!ENTITY"),
        "微信XML无效或过大"
    );
    Ok(roxmltree::Document::parse(value)?)
}

pub fn xml_text(value: &str, name: &str) -> String {
    xml(value)
        .ok()
        .and_then(|doc| {
            doc.descendants()
                .find(|n| n.has_tag_name(name))
                .and_then(|n| n.text())
                .map(str::to_owned)
        })
        .unwrap_or_default()
}

pub fn system_message(row: &Value) -> bool {
    text(row, "type").starts_with("系统")
        || (text(row, "type") == "文件/链接/卡片" && xml_text(text(row, "content"), "type") == "62")
}

fn hash(value: &Value, python_spaces: bool) -> Result<String> {
    let encoded = serde_json::to_string(value)?;
    let encoded = if python_spaces {
        let (mut quoted, mut escaped) = (false, false);
        let mut output = String::with_capacity(encoded.len() + 64);
        for ch in encoded.chars() {
            output.push(ch);
            if escaped {
                escaped = false;
                continue;
            }
            if ch == '\\' && quoted {
                escaped = true;
                continue;
            }
            if ch == '"' {
                quoted = !quoted;
            }
            if !quoted && (ch == ',' || ch == ':') {
                output.push(' ');
            }
        }
        output
    } else {
        encoded
    };
    Ok(format!("{:x}", Sha256::digest(encoded.as_bytes())))
}

fn reply_text(message: &Value) -> Result<String> {
    let content = message["text"].as_str().context("文本无效")?;
    let Some(quote) = message.get("quote").filter(|quote| !quote.is_null()) else {
        return Ok(content.to_owned());
    };
    ensure!(
        quote.is_object()
            && !text(quote, "id").is_empty()
            && !text(quote, "sender").is_empty()
            && text(quote, "text").len() <= 24000
            && quote["unavailable"] != true,
        "引用来源无效或已撤回"
    );
    ensure!(
        !text(message, "_quote_source").is_empty(),
        "引用原文尚未获微信确认"
    );
    Ok(content.to_owned())
}

#[derive(Clone, Debug)]
pub struct Step {
    pub expected: Value,
    pub path: Option<PathBuf>,
}

pub trait Backend {
    fn catalog(&mut self) -> Result<Vec<Value>>;
    fn members(&mut self, group: &str) -> Result<Vec<Value>>;
    fn recent(&mut self, group: &str, limit: usize) -> Result<Vec<Value>>;
    fn page(&mut self, group: &str, position: &Value, limit: usize) -> Result<Vec<Value>>;
    fn older(
        &mut self,
        _group: &str,
        _before: Option<&Value>,
        _limit: usize,
    ) -> Result<Vec<Value>> {
        anyhow::bail!("本机微信历史分页尚未实现")
    }
    fn reload(&mut self, group: &str, locator: &Value) -> Result<Value>;
    fn signatures(&mut self) -> Result<Value>;
    fn authorize(&mut self, groups: &BTreeSet<String>) -> Result<()>;
    fn convert(&mut self, group: &str, row: &Value) -> Result<Option<Value>>;
    fn prepare_media(&mut self, data: &Path, id: &str, index: usize, media: &Value)
    -> Result<Step>;
    fn prepare_quote(
        &mut self,
        _group: &str,
        _source: &str,
        _text: &str,
        _atlist: &str,
    ) -> Result<Step> {
        anyhow::bail!("本机原生引用组件不可用，未发送")
    }
    fn submit(&mut self, group: &str, step: &Step, key: &str) -> Result<()>;
    fn matches(&mut self, group: &str, row: &Value, expected: &Value) -> Result<bool>;
    fn server_id(&mut self, group: &str, row: &Value) -> Result<String>;
    fn recall_info(&mut self, _group: &str, _source: &str, _now: f64) -> Result<Value> {
        anyhow::bail!("本机撤回组件尚未连接")
    }
    fn recall_submit(&mut self, _group: &str, _info: &Value, _request_id: &str) -> Result<()> {
        anyhow::bail!("本机撤回组件尚未连接")
    }
    fn recall_progress(
        &mut self,
        _group: &str,
        _info: &Value,
        _request_id: &str,
    ) -> Result<Option<bool>> {
        Ok(None)
    }
    fn release(&mut self);
}

pub struct Receipts {
    pub db: Connection,
}
impl Receipts {
    pub fn open(path: &Path) -> Result<Self> {
        let db = Connection::open(path)?;
        db.execute_batch("PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY,value TEXT);
            CREATE TABLE IF NOT EXISTS echoes(key TEXT PRIMARY KEY);
            CREATE TABLE IF NOT EXISTS pending(source TEXT PRIMARY KEY,group_id TEXT,locator TEXT,due REAL DEFAULT 0);
            CREATE TABLE IF NOT EXISTS delivered(source TEXT PRIMARY KEY,group_id TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS baseline(source TEXT PRIMARY KEY,group_id TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS pending_due ON pending(due);
            CREATE INDEX IF NOT EXISTS pending_group_due ON pending(group_id,due);")?;
        Ok(Self { db })
    }
    pub fn state(&self, key: &str) -> Result<Option<Value>> {
        use rusqlite::OptionalExtension;
        let row: Option<String> = self
            .db
            .query_row("SELECT value FROM state WHERE key=?", [key], |r| r.get(0))
            .optional()?;
        row.map(|v| serde_json::from_str(&v).map_err(Into::into))
            .transpose()
    }
    pub fn set(&self, key: &str, value: &Value) -> Result<()> {
        self.db.execute(
            "INSERT OR REPLACE INTO state VALUES(?,?)",
            params![key, value.to_string()],
        )?;
        Ok(())
    }
    fn contains(&self, table: &str, column: &str, key: &str) -> Result<bool> {
        Ok(self.db.query_row(
            &format!("SELECT EXISTS(SELECT 1 FROM {table} WHERE {column}=?)"),
            [key],
            |r| r.get(0),
        )?)
    }
    fn echo(&self, key: &str) -> Result<()> {
        self.db
            .execute("INSERT OR IGNORE INTO echoes VALUES(?)", [key])?;
        Ok(())
    }
    fn submitted(&self, id: &str) -> Result<bool> {
        let prefix = format!("wx_attempt:{id}:");
        let mut statement = self
            .db
            .prepare("SELECT value FROM state WHERE substr(key,1,?)=?")?;
        for value in statement.query_map(params![prefix.len() as i64, prefix], |r| {
            r.get::<_, String>(0)
        })? {
            if serde_json::from_str::<Value>(&value?)?["rejected"] != true {
                return Ok(true);
            }
        }
        Ok(false)
    }
    fn acknowledge(&self, group: &str, key: &str) -> Result<()> {
        let tx = self.db.unchecked_transaction()?;
        tx.execute("INSERT OR IGNORE INTO delivered SELECT source,group_id FROM pending WHERE source=? AND group_id=?", params![key, group])?;
        tx.execute(
            "DELETE FROM pending WHERE source=? AND group_id=?",
            params![key, group],
        )?;
        tx.commit()?;
        Ok(())
    }
}

pub fn selection(value: &Value, catalog: &[Value]) -> Result<BTreeSet<String>> {
    let array = value.as_array().context("转发群配置无效，需要群编号数组")?;
    ensure!(array.len() <= 4096, "转发群过多");
    let known: BTreeSet<&str> = catalog.iter().map(|g| text(g, "id")).collect();
    let mut result = BTreeSet::new();
    for group in array {
        let group = group.as_str().context("转发群编号无效")?;
        ensure!(
            !group.is_empty()
                && group.chars().count() <= 128
                && !group.chars().any(char::is_control),
            "转发群编号无效"
        );
        if known.contains(group) {
            result.insert(group.to_owned());
        }
    }
    Ok(result)
}

pub fn position(row: &Value) -> Result<Value> {
    Ok(
        json!({"sort_seq":row["sort_seq"].as_i64().context("消息游标无效")?,
        "shard":row["shard"].as_str().context("消息分片无效")?,
        "local_id":row["local_id"].as_i64().context("消息编号无效")?}),
    )
}

fn journal(store: &Receipts, group: &str, row: &Value) -> Result<bool> {
    let key = source(group, row)?;
    if system_message(row)
        || store.contains("echoes", "key", &key)?
        || store.contains("delivered", "source", &key)?
    {
        return Ok(false);
    }
    Ok(store.db.execute(
        "INSERT OR IGNORE INTO pending(source,group_id,locator) VALUES(?,?,?)",
        params![key, group, position(row)?.to_string()],
    )? > 0)
}

fn initialize<A: Backend>(
    access: &mut A,
    store: &Receipts,
    groups: &BTreeSet<String>,
) -> Result<()> {
    for group in groups {
        if store.state(&format!("cursor:{group}"))?.is_some() {
            continue;
        }
        let latest = access.recent(group, 1)?;
        let floor = latest
            .iter()
            .map(|m| m["sort_seq"].as_i64().context("消息游标无效"))
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .max()
            .unwrap_or(0);
        let tx = store.db.unchecked_transaction()?;
        if !latest.is_empty() {
            let mut cursor = json!({"sort_seq":floor-1,"shard":"","local_id":-1});
            loop {
                let rows = access.page(group, &cursor, 200)?;
                for row in &rows {
                    if row["sort_seq"].as_i64().context("消息游标无效")? == floor {
                        tx.execute(
                            "INSERT OR IGNORE INTO baseline VALUES(?,?)",
                            params![source(group, row)?, group],
                        )?;
                    }
                }
                let more = rows.len() == 200
                    && rows.last().unwrap()["sort_seq"]
                        .as_i64()
                        .context("消息游标无效")?
                        <= floor;
                if !more {
                    break;
                }
                cursor = position(rows.last().unwrap())?;
            }
        }
        store.set(&format!("cursor:{group}"), &json!(floor))?;
        store.set(&format!("watch_floor:{group}"), &json!(floor))?;
        tx.commit()?;
    }
    Ok(())
}

fn backfill<A: Backend>(access: &mut A, store: &Receipts, group: &str) -> Result<()> {
    let key = format!("history_loaded:{group}");
    if store.state(&key)? == Some(json!(true)) {
        return Ok(());
    }
    let rows = access.recent(group, 40)?;
    let tx = store.db.unchecked_transaction()?;
    for row in rows.iter().rev() {
        if journal(store, group, row)? {
            // A warm history read is not a live arrival, even if media becomes ready later.
            // An existing pending row may already belong to the live watcher; preserve its origin.
            let mut locator = position(row)?;
            locator["historical"] = json!(true);
            store.db.execute(
                "UPDATE pending SET locator=?1 WHERE source=?2 AND group_id=?3",
                params![locator.to_string(), source(group, row)?, group],
            )?;
        }
    }
    store.set(&key, &json!(true))?;
    tx.commit()?;
    Ok(())
}

fn history_position(group: &str, source: &str) -> Result<Option<Value>> {
    if source.is_empty() {
        return Ok(None);
    }
    let rest = source
        .strip_prefix(&format!("wx:{group}:"))
        .context("历史游标不属于该群")?;
    let (rest, sequence) = rest.rsplit_once(':').context("历史游标无效")?;
    let (shard, local) = rest.rsplit_once(':').context("历史游标无效")?;
    ensure!(
        !shard.is_empty() && shard.len() <= 256 && !shard.chars().any(char::is_control),
        "历史分片无效"
    );
    Ok(Some(
        json!({"sort_seq":sequence.parse::<i64>().context("历史游标无效")?,"shard":shard,"local_id":local.parse::<i64>().context("历史定位无效")?}),
    ))
}

fn history_page<A: Backend>(
    access: &mut A,
    store: &Receipts,
    command: &Value,
    now: f64,
    events: &mut Vec<Value>,
    inflight: Option<&Job>,
) -> Result<()> {
    let group = text(command, "group");
    let request = text(command, "request_id");
    uuid::Uuid::parse_str(request).context("历史请求编号无效")?;
    ensure!(command["limit"] == 60, "历史页容量无效");
    let before = history_position(group, text(command, "before_source"))?;
    let mut rows = access.older(group, before.as_ref(), 61)?;
    ensure!(rows.len() <= 61, "本机历史页过大");
    let has_more = rows.len() > 60;
    rows.truncate(60);
    let before_source = rows
        .last()
        .map(|row| source(group, row))
        .transpose()?
        .unwrap_or_else(|| text(command, "before_source").to_owned());
    let mut sources = Vec::new();
    let mut media_budget = 8;
    for row in rows.iter().rev() {
        if system_message(row) {
            continue;
        }
        let key = source(group, row)?;
        if store.contains("echoes", "key", &key)? {
            continue;
        }
        sources.push(key.clone());
        journal(store, group, row)?;
        let mut locator = position(row)?;
        locator["historical"] = json!(true);
        store.db.execute(
            "UPDATE pending SET locator=?1 WHERE source=?2",
            params![locator.to_string(), key],
        )?;
        if let Some(job) = inflight
            && job.defers(access, store, group, row)?
        {
            continue;
        }
        let plain = text(row, "type") == "文本";
        if !plain {
            if media_budget == 0 || store.contains("delivered", "source", &key)? {
                continue;
            }
            media_budget -= 1;
        }
        match access.convert(group, row) {
            Ok(Some(message)) => {
                events.push(json!({"type":"message","historical":true,"message":message}))
            }
            Ok(None) => {
                sources.pop();
                store
                    .db
                    .execute("DELETE FROM pending WHERE source=?", [&key])?;
            }
            Err(_) => {} // Keep the durable locator for the normal bounded media retry path.
        }
        store.db.execute(
            "UPDATE pending SET due=? WHERE source=?",
            params![now + 30.0, key],
        )?;
    }
    events.push(json!({"type":"history_done","group":group,"request_id":request,"before_source":before_source,"has_more":has_more,"sources":sources}));
    Ok(())
}

fn journal_page<A: Backend>(
    access: &mut A,
    store: &Receipts,
    group: &str,
    cursor: &Value,
) -> Result<(Value, bool)> {
    let rows = access.page(group, cursor, 200)?;
    let mut floor = store
        .state(&format!("cursor:{group}"))?
        .and_then(|v| v.as_i64())
        .unwrap_or(0);
    let baseline = store
        .state(&format!("watch_floor:{group}"))?
        .and_then(|v| v.as_i64());
    let tx = store.db.unchecked_transaction()?;
    for row in &rows {
        let seq = row["sort_seq"].as_i64().context("消息游标无效")?;
        if baseline.is_none_or(|base| seq >= base)
            && !store.contains("baseline", "source", &source(group, row)?)?
        {
            journal(store, group, row)?;
        }
        floor = floor.max(seq);
    }
    store.set(&format!("cursor:{group}"), &json!(floor))?;
    tx.commit()?;
    Ok((
        rows.last()
            .map(position)
            .transpose()?
            .unwrap_or_else(|| cursor.clone()),
        rows.len() == 200,
    ))
}

struct Job {
    message: Value,
    steps: Vec<Step>,
    record: Value,
    index: usize,
    folder: PathBuf,
}
impl Job {
    fn new<A: Backend>(
        access: &mut A,
        store: &Receipts,
        data: &Path,
        directory: &Path,
        message: &Value,
    ) -> Result<Self> {
        let id = text(message, "id");
        let uuid = uuid::Uuid::parse_str(id).context("消息编号不是UUID")?;
        ensure!(uuid.to_string() == id, "消息编号不是标准UUID");
        let mut expected = json!({"group":message["group"],"text":message["text"],"mentions":message["mentions"],"media":message["media"]});
        if let Some(quote) = message.get("quote").filter(|quote| !quote.is_null()) {
            expected["quote"] = quote.clone();
        }
        let fingerprint = hash(&expected, true)?;
        let mut record = store
            .state(&format!("outgoing:{id}"))?
            .unwrap_or_else(|| json!({"hash":fingerprint}));
        ensure!(
            record["hash"] == fingerprint,
            "重复发送编号的内容不一致，未重新提交"
        );
        let folder = directory.join("feishu/media").join(id);
        if record.get("ack").is_some() {
            return Ok(Self {
                message: message.clone(),
                steps: vec![],
                record,
                index: 0,
                folder,
            });
        }
        let mentions = message["mentions"].as_array().context("@成员列表无效")?;
        let media = message["media"].as_array().context("附件列表无效")?;
        ensure!(
            mentions.len() <= 100 && media.len() <= 12,
            "附件或@成员过多"
        );
        let roster = access.members(text(message, "group"))?;
        let original_content = message["text"].as_str().context("文本无效")?;
        let composed = reply_text(message)?;
        let content = composed.as_str();
        ensure!(
            message.get("quote").is_none_or(|quote| quote.is_null()) || !content.trim().is_empty(),
            "引用回复请填写文字"
        );
        ensure!(
            content.len() <= 24000 && !content.contains('\0'),
            "微信文本超过容量或含无效字符"
        );
        let mut at = vec![];
        for mention in mentions {
            let member = roster
                .iter()
                .find(|m| m["id"] == mention["id"] && m["name"] == mention["name"]);
            ensure!(
                member.is_some()
                    && !text(mention, "id").is_empty()
                    && original_content.contains(&format!("@{}", text(mention, "name"))),
                "@成员已变化，请重新选择"
            );
            if !at.contains(&text(mention, "id")) {
                at.push(text(mention, "id"));
            }
        }
        let mut steps = vec![];
        if !content.trim().is_empty() {
            steps.push(
                if message.get("quote").is_some_and(|quote| !quote.is_null()) {
                    access.prepare_quote(
                        text(message, "group"),
                        text(message, "_quote_source"),
                        content,
                        &at.join(","),
                    )?
                } else {
                    Step {
                        expected: json!({"kind":"text","text":content,"atlist":at.join(",")}),
                        path: None,
                    }
                },
            );
        }
        // Verify every attachment before the first native submission.
        for (index, item) in media.iter().enumerate() {
            steps.push(access.prepare_media(data, id, index, item)?);
        }
        ensure!(!steps.is_empty(), "消息为空");
        let index = record["index"].as_u64().unwrap_or(0) as usize;
        ensure!(index <= steps.len(), "保存的发送进度无效，未重新发送");
        record["hash"] = json!(fingerprint);
        store.set(&format!("outgoing:{id}"), &record)?;
        Ok(Self {
            message: message.clone(),
            steps,
            record,
            index,
            folder,
        })
    }
    fn save(&self, store: &Receipts) -> Result<()> {
        store.set(
            &format!("outgoing:{}", text(&self.message, "id")),
            &self.record,
        )
    }
    fn attempt_key(&self) -> String {
        format!("wx_attempt:{}:{}", text(&self.message, "id"), self.index)
    }
    fn finish(&mut self, store: &Receipts, status: &str, error: &str) -> Result<Value> {
        let id = text(&self.message, "id");
        let mut sources = vec![];
        let mut timestamp = None;
        for index in 0..self.steps.len() {
            if let Some(attempt) = store.state(&format!("wx_attempt:{id}:{index}"))? {
                if !text(&attempt, "ack").is_empty() {
                    sources.push(text(&attempt, "ack").to_owned());
                    if timestamp.is_none() {
                        timestamp = attempt["create_time"]
                            .as_i64()
                            .and_then(|stamp| stamp.checked_mul(1000));
                    }
                }
            }
        }
        let mut ack = json!({"type":"ack","id":id,"status":status,"source_id":sources.first().cloned().unwrap_or_default(),"source_ids":sources});
        if !error.is_empty() {
            ack["error"] = json!(error);
        }
        if status == "sent" {
            if let Some(timestamp) = timestamp {
                ack["timestamp"] = json!(timestamp);
            }
        }
        self.record["ack"] = ack.clone();
        self.save(store)?;
        if (status == "sent" || !store.submitted(id)?) && self.folder.is_dir() {
            let _ = std::fs::remove_dir_all(&self.folder);
        }
        Ok(ack)
    }
    fn cancel(&mut self, store: &Receipts) -> Result<Option<Value>> {
        if let Some(ack) = self.record.get("ack") {
            return Ok(Some(ack.clone()));
        }
        self.record["cancelled"] = json!(true);
        self.save(store)?;
        let attempt = store.state(&self.attempt_key())?;
        if attempt.is_some_and(|v| v["rejected"] != true) {
            return Ok(None);
        }
        let status = if store.submitted(text(&self.message, "id"))? {
            "uncertain"
        } else {
            "failed"
        };
        Ok(Some(self.finish(
            store,
            status,
            "该群已取消转发，后续内容未提交微信",
        )?))
    }
    fn advance<A: Backend>(
        &mut self,
        access: &mut A,
        store: &Receipts,
        now: f64,
    ) -> Result<Option<Value>> {
        if let Some(ack) = self.record.get("ack") {
            return Ok(Some(ack.clone()));
        }
        let cancelled = self.record["cancelled"] == true;
        if self.index == self.steps.len() {
            return Ok(Some(self.finish(store, "sent", "")?));
        }
        let existing = store.state(&self.attempt_key())?;
        if cancelled && existing.as_ref().is_none_or(|v| v["rejected"] == true) {
            return self.cancel(store);
        }
        let result = self.check_step(access, store, now, existing);
        match result {
            Ok(false) => Ok(None),
            Ok(true) => {
                self.index += 1;
                self.record["index"] = json!(self.index);
                self.save(store)?;
                if cancelled && self.index < self.steps.len() {
                    return Ok(Some(self.finish(
                        store,
                        "uncertain",
                        "该群已取消转发；已提交部分得到确认，后续内容未提交微信",
                    )?));
                }
                if self.index == self.steps.len() {
                    return Ok(Some(self.finish(store, "sent", "")?));
                }
                Ok(None)
            }
            Err(error) => {
                let status = if store.submitted(text(&self.message, "id"))? {
                    "uncertain"
                } else {
                    "failed"
                };
                Ok(Some(self.finish(store, status, &error.to_string())?))
            }
        }
    }
    fn check_step<A: Backend>(
        &mut self,
        access: &mut A,
        store: &Receipts,
        now: f64,
        existing: Option<Value>,
    ) -> Result<bool> {
        let step = &self.steps[self.index];
        let mut expected = step.expected.clone();
        let flags: Vec<_> = ["gif_attachment", "video_attachment"]
            .iter()
            .filter_map(|name| {
                expected
                    .as_object_mut()
                    .unwrap()
                    .remove(*name)
                    .map(|value| ((*name).to_owned(), value))
            })
            .collect();
        let expected_hash = hash(&expected, false)?;
        let mut state = existing.unwrap_or(Value::Null);
        if !state.is_null() {
            ensure!(
                state["expected_hash"] == expected_hash,
                "重试内容或@成员发生变化，请先核对原消息"
            );
            if !text(&state, "ack").is_empty() {
                store.echo(text(&state, "ack"))?;
                return Ok(true);
            }
        }
        let group = text(&self.message, "group");
        if state.is_null() || state["rejected"] == true {
            let before = access
                .recent(group, 1)?
                .iter()
                .filter_map(|row| row["sort_seq"].as_i64())
                .max()
                .unwrap_or(0);
            let mut matching = expected.clone();
            for (name, value) in flags {
                matching[&name] = value;
            }
            state = json!({"expected_hash":expected_hash,"before":before,"group":group,"expected":matching,"await_started":now});
            store.set(&self.attempt_key(), &state)?;
            if let Err(error) = access.submit(
                group,
                step,
                &format!("{}:{}", text(&self.message, "id"), self.index),
            ) {
                if error.downcast_ref::<Rejected>().is_some() {
                    state["rejected"] = json!(true);
                    store.set(&self.attempt_key(), &state)?;
                }
                return Err(error);
            }
            state["response"] = json!({"status":"queued","ret":0});
            store.set(&self.attempt_key(), &state)?;
        }
        if state.get("await_started").is_none() {
            state["await_started"] = json!(now);
            store.set(&self.attempt_key(), &state)?;
        }
        let before = state["before"]
            .as_i64()
            .context("已提交消息的确认游标无效")?;
        let rows = access.page(
            group,
            &json!({"sort_seq":before,"shard":"\u{10ffff}","local_id":i64::MAX}),
            2000,
        )?;
        let mut matches = vec![];
        for row in &rows {
            if access.matches(group, row, &state["expected"])? {
                let id = access.server_id(group, row)?;
                if !id.is_empty() && id != "0" {
                    matches.push((source(group, row)?, id, row["create_time"].clone()));
                }
            }
        }
        ensure!(
            matches.len() <= 1,
            "微信出现多条相同内容，不能唯一确认发送，未自动重发"
        );
        if let Some((ack, id, created)) = matches.first() {
            state["ack"] = json!(ack);
            state["server_id"] = json!(id);
            if created.as_i64().is_some() {
                state["create_time"] = created.clone();
            }
            store.echo(ack)?;
            store.set(&self.attempt_key(), &state)?;
            return Ok(true);
        }
        let timeout = if text(&expected, "kind") == "text" {
            30.0
        } else {
            120.0
        };
        ensure!(
            now - state["await_started"]
                .as_f64()
                .context("发送确认时间无效")?
                < timeout,
            "微信消息已提交，但服务端确认尚未回读；请查看目标群，未自动重发"
        );
        Ok(false)
    }
    fn defers<A: Backend>(
        &self,
        access: &mut A,
        store: &Receipts,
        group: &str,
        row: &Value,
    ) -> Result<bool> {
        if text(&self.message, "group") != group {
            return Ok(false);
        }
        let Some(state) = store.state(&self.attempt_key())? else {
            return Ok(false);
        };
        if state.get("expected").is_none()
            || !text(&state, "ack").is_empty()
            || state["rejected"] == true
            || row["sort_seq"].as_i64().unwrap_or(0) <= state["before"].as_i64().unwrap_or(i64::MAX)
        {
            return Ok(false);
        }
        access.matches(group, row, &state["expected"])
    }
}

#[derive(Debug)]
pub struct Rejected;
impl std::fmt::Display for Rejected {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("微信组件拒绝了发送请求，消息已保留")
    }
}
impl std::error::Error for Rejected {}

pub struct Engine<A: Backend> {
    pub access: A,
    pub store: Receipts,
    pub catalog: Vec<Value>,
    pub watched: BTreeSet<String>,
    selected: BTreeSet<String>,
    active: BTreeSet<String>,
    jobs: Vec<Job>,
    recalls: Vec<crate::recall::RecallJob>,
    pages: HashMap<String, Value>,
    signatures: Option<Value>,
    warned: BTreeSet<String>,
    data: PathBuf,
    directory: PathBuf,
    last_poll: f64,
    last_groups: f64,
    last_members: f64,
    last_media: f64,
    last_send: f64,
}
impl<A: Backend> Engine<A> {
    pub fn new(mut access: A, data: PathBuf, policy: &Value, now: f64) -> Result<Self> {
        let directory = data.join("adapter");
        std::fs::create_dir_all(&directory)?;
        let store = Receipts::open(&directory.join("receipts.sqlite"))?;
        let catalog = access.catalog()?;
        let selected = selection(policy, &catalog)?;
        let result = initialize(&mut access, &store, &selected);
        access.release();
        result?;
        Ok(Self {
            access,
            store,
            catalog,
            watched: selected.clone(),
            selected,
            active: BTreeSet::new(),
            jobs: vec![],
            recalls: vec![],
            pages: HashMap::new(),
            signatures: None,
            warned: BTreeSet::new(),
            data,
            directory,
            last_poll: 0.0,
            last_groups: now,
            last_members: 0.0,
            last_media: 0.0,
            last_send: 0.0,
        })
    }
    pub fn status(&self) -> Value {
        json!({"type":"status","ready":true,"text":format!("已连接本机 Windows 微信 · Rust 适配器 · 已选择 {} 个群转发",self.watched.len())})
    }
    fn authorize(&mut self, group: &str) -> Result<()> {
        ensure!(
            self.watched.contains(group),
            "该群未在本机服务端选择转发，不能发送"
        );
        if !self.active.contains(group) {
            let mut next = self.active.clone();
            next.insert(group.to_owned());
            self.access.authorize(&next)?;
            self.active = next;
        }
        Ok(())
    }
    fn configure(&mut self, selected: BTreeSet<String>, events: &mut Vec<Value>) -> Result<()> {
        let changed = selected != self.watched;
        self.watched.retain(|g| selected.contains(g));
        self.active.retain(|g| selected.contains(g));
        self.access.authorize(&self.active)?;
        let mut index = 0;
        while index < self.jobs.len() {
            let ack = if !selected.contains(text(&self.jobs[index].message, "group")) {
                self.jobs[index].cancel(&self.store)?
            } else {
                None
            };
            if let Some(ack) = ack {
                events.push(ack);
                self.jobs.remove(index);
            } else {
                index += 1;
            }
        }
        self.selected = selected.clone();
        self.pages.retain(|g, _| selected.contains(g));
        if changed {
            self.signatures = None;
        }
        initialize(&mut self.access, &self.store, &selected)?;
        self.watched = selected;
        Ok(())
    }
    pub fn cycle(&mut self, commands: &[Value], now: f64) -> Vec<Value> {
        let mut events = vec![];
        let result = self.cycle_inner(commands, now, &mut events);
        self.access.release();
        if let Err(error) = result {
            events.push(json!({"type":"warning","text":error.to_string()}));
        }
        events
    }
    fn cycle_inner(&mut self, commands: &[Value], now: f64, events: &mut Vec<Value>) -> Result<()> {
        let poll_due = !self.watched.is_empty() && now - self.last_poll >= 1.4;
        let signature = if poll_due {
            Some(self.access.signatures()?)
        } else {
            None
        };
        // Apply queued revocations before advancing any native send.
        for command in commands {
            let operation = text(command, "op");
            let result = match operation {
                "activate" => (|| {
                    let group = text(command, "group");
                    self.authorize(group)?;
                    backfill(&mut self.access, &self.store, group)?;
                    events.push(json!({"type":"members","group":group,"members":self.access.members(group)?}));
                    Ok(())
                })(),
                "configure" => (|| {
                    let next = selection(&command["groups"], &self.catalog)?;
                    self.configure(next, events)?;
                    events.push(self.status());
                    Ok(())
                })(),
                "history" => (|| {
                    let group = text(command, "group");
                    ensure!(self.watched.contains(group), "该群未获准读取历史");
                    history_page(
                        &mut self.access,
                        &self.store,
                        command,
                        now,
                        events,
                        self.jobs.first(),
                    )
                })(),
                "receipt" => {
                    self.warned.remove(text(command, "source_id"));
                    self.store
                        .acknowledge(text(command, "group"), text(command, "source_id"))
                }
                "send" => (|| {
                    let message = &command["message"];
                    let id = text(message, "id");
                    if self.jobs.iter().any(|job| text(&job.message, "id") == id) {
                        return Ok(());
                    }
                    ensure!(self.jobs.len() < 128, "微信发送队列已满");
                    self.authorize(text(message, "group"))?;
                    self.jobs.push(Job::new(
                        &mut self.access,
                        &self.store,
                        &self.data,
                        &self.directory,
                        message,
                    )?);
                    Ok(())
                })(),
                "recall" | "recall_check" => (|| {
                    let message = &command["message"];
                    let id = text(message, "id");
                    if self.recalls.iter().any(|job| job.id() == id) {
                        return Ok(());
                    }
                    ensure!(self.recalls.len() < 128, "微信撤回队列已满");
                    let check_only = operation == "recall_check";
                    if !check_only {
                        self.authorize(text(message, "group"))?;
                    }
                    let job = crate::recall::RecallJob::new(
                        &mut self.access,
                        &self.store,
                        message,
                        check_only,
                        now,
                    )?;
                    self.recalls.push(job);
                    Ok(())
                })(),
                _ => Ok(()),
            };
            if let Err(error) = result {
                if operation == "send" {
                    let id = text(&command["message"], "id");
                    events.push(json!({"type":"ack","id":id,"status":if self.store.submitted(id)? {"uncertain"} else {"failed"},"error":error.to_string()}));
                } else if operation == "history" {
                    events.push(json!({"type":"history_done","group":command["group"],"request_id":command["request_id"],"error":error.to_string()}));
                } else if operation == "recall" || operation == "recall_check" {
                    events.push(json!({"type":"recall_ack","id":command["message"]["id"],"status":"failed","error":error.to_string()}));
                } else {
                    let mut warning = json!({"type":"warning","text":error.to_string()});
                    if operation == "activate" {
                        warning["group"] = command["group"].clone();
                    }
                    events.push(warning);
                }
            }
        }
        if !self.jobs.is_empty() && now - self.last_send >= 1.4 {
            self.last_send = now;
            let job = &mut self.jobs[0];
            let mut result = if self.watched.contains(text(&job.message, "group")) {
                None
            } else {
                job.cancel(&self.store)?
            };
            if result.is_none() {
                result = job.advance(&mut self.access, &self.store, now)?;
            }
            if let Some(ack) = result {
                events.push(ack);
                self.jobs.remove(0);
            }
        }
        if let Some(job) = self.recalls.first_mut() {
            if let Some(ack) = job.advance(
                &mut self.access,
                &self.store,
                self.watched.contains(job.group()),
                now,
            )? {
                events.push(ack);
                self.recalls.remove(0);
            }
        }
        if now - self.last_groups >= 60.0 {
            let catalog = self.access.catalog()?;
            let selected = selection(&json!(self.selected), &catalog)?;
            self.configure(selected, events)?;
            if catalog != self.catalog {
                self.catalog = catalog;
                events.push(json!({"type":"groups","groups":self.catalog}));
            }
            self.last_groups = now;
        }
        if poll_due {
            self.last_poll = now;
            if signature != self.signatures {
                let mut more = false;
                for group in &self.watched {
                    let default = json!({"sort_seq":self.store.state(&format!("cursor:{group}"))?.and_then(|v|v.as_i64()).unwrap_or(0)-1,"shard":"","local_id":-1});
                    let (next, remaining) = journal_page(
                        &mut self.access,
                        &self.store,
                        group,
                        self.pages.get(group).unwrap_or(&default),
                    )?;
                    self.pages.insert(group.clone(), next);
                    more |= remaining;
                }
                if !more {
                    self.signatures = signature;
                    self.pages.clear();
                }
            }
        }
        if !self.active.is_empty() && now - self.last_members > 60.0 {
            for group in &self.active {
                events.push(
                    json!({"type":"members","group":group,"members":self.access.members(group)?}),
                );
            }
            self.last_members = now;
        }
        if !self.watched.is_empty() && now - self.last_media >= 1.4 {
            self.last_media = now;
            let placeholders = std::iter::repeat_n("?", self.watched.len())
                .collect::<Vec<_>>()
                .join(",");
            let mut parameters = vec![rusqlite::types::Value::Real(now)];
            parameters.extend(
                self.watched
                    .iter()
                    .cloned()
                    .map(rusqlite::types::Value::Text),
            );
            let mut statement=self.store.db.prepare(&format!("SELECT source,group_id,locator FROM pending WHERE due<? AND group_id IN ({placeholders}) ORDER BY due,rowid LIMIT 8"))?;
            let rows = statement.query_map(rusqlite::params_from_iter(parameters), |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                ))
            })?;
            let mut pending = vec![];
            for row in rows {
                pending.push(row?);
            }
            drop(statement);
            for (key, group, locator) in pending {
                if self.store.contains("echoes", "key", &key)? {
                    self.store
                        .db
                        .execute("DELETE FROM pending WHERE source=?", [&key])?;
                    continue;
                }
                let result: Result<bool> = (|| {
                    let locator: Value = serde_json::from_str(&locator)?;
                    let row = self.access.reload(&group, &locator)?;
                    if let Some(job) = self.jobs.first() {
                        if job.defers(&mut self.access, &self.store, &group, &row)? {
                            return Ok(false);
                        }
                    }
                    if let Some(message) = self.access.convert(&group, &row)? {
                        events.push(json!({"type":"message","historical":locator["historical"]==true,"message":message}));
                    } else {
                        self.store
                            .db
                            .execute("DELETE FROM pending WHERE source=?", [&key])?;
                    }
                    Ok(true)
                })();
                let delay = match result {
                    Ok(false) => 2.0,
                    Ok(true) => 30.0,
                    Err(error) => {
                        if self.warned.insert(key.clone()) {
                            events.push(json!({"type":"warning","group":group,"text":format!("微信附件暂未就绪，将保留并重试：{error}")}));
                        }
                        30.0
                    }
                };
                self.store.db.execute(
                    "UPDATE pending SET due=? WHERE source=?",
                    params![now + delay, key],
                )?;
            }
        }
        Ok(())
    }
    pub fn shutdown(&mut self) -> Result<()> {
        self.access.authorize(&BTreeSet::new())?;
        self.access.release();
        Ok(())
    }
}

#[cfg(test)]
mod tests;
