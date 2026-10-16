use crate::{
    access::{ClientRecord, Clients},
    model::{ChatMessage, Group, Media, Member, Quote},
};
use anyhow::{Context, Result, bail};
use rusqlite::{Connection, OptionalExtension, params};
use serde_json::{Value, json};
use std::{collections::BTreeSet, path::Path, sync::Mutex};

pub struct Store {
    db: Mutex<Connection>,
}

impl Store {
    pub fn open(path: &Path) -> Result<Self> {
        let db = Connection::open(path)?;
        db.execute_batch("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-4096;
            CREATE TABLE IF NOT EXISTS groups(id TEXT PRIMARY KEY, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS members(group_id TEXT PRIMARY KEY, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS media(id TEXT PRIMARY KEY, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS media_owners(media_id TEXT PRIMARY KEY,group_id TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS messages(seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
                group_id TEXT NOT NULL, source_id TEXT NOT NULL, body TEXT NOT NULL, status TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS messages_group ON messages(group_id,seq);
            CREATE INDEX IF NOT EXISTS messages_time ON messages(group_id,CAST(json_extract(body,'$.timestamp') AS INTEGER),seq);
            CREATE INDEX IF NOT EXISTS messages_pending ON messages(status,seq);
            CREATE INDEX IF NOT EXISTS messages_recall ON messages(json_extract(body,'$.recall_status'),seq);
            CREATE UNIQUE INDEX IF NOT EXISTS messages_source ON messages(group_id,source_id) WHERE source_id!='';
            CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS access_clients(id TEXT PRIMARY KEY, token_hash TEXT UNIQUE NOT NULL, body TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS history_sources(group_id TEXT NOT NULL, source_id TEXT NOT NULL, PRIMARY KEY(group_id,source_id));
            CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY, body TEXT NOT NULL);")?;
        let store = Self { db: Mutex::new(db) };
        let pending: Vec<ChatMessage> = {
            let conn = store.db.lock().unwrap();
            let mut stmt = conn.prepare("SELECT body FROM messages WHERE status='sending'")?;
            stmt.query_map([], |r| r.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?
                .into_iter()
                .map(|s| serde_json::from_str(&s))
                .collect::<Result<_, _>>()?
        };
        for mut message in pending {
            message.status = "uncertain".into();
            message.error = "服务在微信确认前退出，请先核对微信群，未自动重发".into();
            store.update_message(&message)?;
        }
        Ok(store)
    }

    pub fn clients(&self) -> Result<Clients> {
        let conn = self.db.lock().unwrap();
        let mut stmt = conn.prepare("SELECT body,token_hash FROM access_clients ORDER BY id")?;
        stmt.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })?
        .map(|row| {
            let (body, token_hash) = row?;
            let access: crate::access::ClientAccess = serde_json::from_str(&body)?;
            Ok((access.id.clone(), ClientRecord { access, token_hash }))
        })
        .collect()
    }

    pub fn save_client(&self, record: &ClientRecord, create: bool) -> Result<Vec<Value>> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let old: Option<String> = tx
            .query_row(
                "SELECT body FROM access_clients WHERE id=?1",
                [&record.access.id],
                |row| row.get(0),
            )
            .optional()?;
        let validate_groups;
        if create {
            validate_groups = true;
            if old.is_some() {
                bail!("客户端已存在");
            }
            let count: i64 =
                tx.query_row("SELECT count(*) FROM access_clients", [], |row| row.get(0))?;
            if count >= 256 {
                bail!("客户端数量达到 256 个上限");
            }
        } else {
            let old: crate::access::ClientAccess =
                serde_json::from_str(&old.context("客户端不存在")?)?;
            validate_groups = old.groups != record.access.groups;
            if old.revoked && !record.access.revoked {
                bail!("已撤销的密钥不能恢复，请创建新密钥");
            }
        }
        for group in record.access.groups.iter().filter(|_| validate_groups) {
            let exists: bool = tx.query_row(
                "SELECT EXISTS(SELECT 1 FROM groups WHERE id=?1)",
                [group],
                |row| row.get(0),
            )?;
            if !exists {
                bail!("只能授权本机微信中存在的群");
            }
        }
        tx.execute("INSERT INTO access_clients(id,token_hash,body) VALUES(?1,?2,?3) ON CONFLICT(id) DO UPDATE SET body=excluded.body", params![record.access.id,record.token_hash,serde_json::to_string(&record.access)?])?;
        let pending = {
            let mut stmt = tx.prepare("SELECT body FROM messages WHERE status='queued' AND json_extract(body,'$.access_client')=?1")?;
            stmt.query_map([&record.access.id], |row| row.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?
        };
        let mut events = Vec::new();
        for body in pending {
            let mut message: ChatMessage = serde_json::from_str(&body)?;
            if record.access.enabled
                && !record.access.revoked
                && record.access.can_send
                && record.access.groups.contains(&message.group)
            {
                continue;
            }
            message.status = "failed".into();
            message.error = "客户端发送权限已取消，未提交微信".into();
            tx.execute(
                "UPDATE messages SET body=?1,status='failed' WHERE id=?2",
                params![serde_json::to_string(&message)?, message.id],
            )?;
            events.push(Self::append_event(&tx, &message, false)?);
        }
        tx.commit()?;
        Ok(events)
    }

    pub fn groups(&self) -> Result<Vec<Group>> {
        let conn = self.db.lock().unwrap();
        let mut stmt = conn.prepare("SELECT body FROM groups ORDER BY rowid")?;
        stmt.query_map([], |r| r.get::<_, String>(0))?
            .map(|s| Ok(serde_json::from_str(&s?)?))
            .collect()
    }

    pub fn forward_groups(&self) -> Result<Option<BTreeSet<String>>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row(
                "SELECT body FROM state WHERE key='forward_groups'",
                [],
                |r| r.get(0),
            )
            .optional()?;
        Ok(body.map(|body| serde_json::from_str(&body)).transpose()?)
    }

    pub fn initialize_forward_groups(&self, selected: &BTreeSet<String>) -> Result<()> {
        self.db.lock().unwrap().execute(
            "INSERT OR IGNORE INTO state(key,body) VALUES('forward_groups',?1)",
            [serde_json::to_string(selected)?],
        )?;
        Ok(())
    }

    pub fn set_forward_groups(&self, selected: &BTreeSet<String>) -> Result<Vec<Value>> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        for group in selected {
            let exists: bool = tx.query_row(
                "SELECT EXISTS(SELECT 1 FROM groups WHERE id=?1)",
                [group],
                |r| r.get(0),
            )?;
            if !exists {
                bail!("只能选择本机微信中存在的群");
            }
        }
        tx.execute(
            "INSERT OR REPLACE INTO state(key,body) VALUES('forward_groups',?1)",
            [serde_json::to_string(selected)?],
        )?;
        let pending = {
            let mut stmt = tx.prepare("SELECT body FROM messages WHERE status='queued'")?;
            stmt.query_map([], |r| r.get::<_, String>(0))?
                .collect::<rusqlite::Result<Vec<_>>>()?
        };
        let mut events = Vec::new();
        for body in pending {
            let mut message: ChatMessage = serde_json::from_str(&body)?;
            if selected.contains(&message.group) {
                continue;
            }
            message.status = "failed".into();
            message.error = "该群已取消转发，未提交微信".into();
            tx.execute(
                "UPDATE messages SET body=?1,status='failed' WHERE id=?2",
                params![serde_json::to_string(&message)?, message.id],
            )?;
            events.push(Self::append_event(&tx, &message, false)?);
        }
        tx.commit()?;
        Ok(events)
    }

    pub fn group_summaries(&self) -> Result<Vec<Value>> {
        let conn = self.db.lock().unwrap();
        let mut stmt = conn.prepare(
            "SELECT g.body,(SELECT m.body FROM messages m WHERE m.group_id=g.id ORDER BY CAST(json_extract(m.body,'$.timestamp') AS INTEGER) DESC,m.seq DESC LIMIT 1)
             FROM groups g ORDER BY g.rowid",
        )?;
        stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?))
        })?
        .map(|row| {
            let (group, last) = row?;
            let mut group: Value = serde_json::from_str(&group)?;
            let message: Option<ChatMessage> =
                last.map(|body| serde_json::from_str(&body)).transpose()?;
            let mut preview = String::new();
            let mut timestamp = 0;
            if let Some(message) = message {
                timestamp = message.timestamp;
                let body = if message.status == "recalled" {
                    "撤回了一条消息".into()
                } else if message.text.trim().is_empty() {
                    message
                        .media
                        .first()
                        .map(|media| {
                            let kind = match media.kind.as_str() {
                                "image" => "图片",
                                "gif" => "GIF",
                                "video" => "视频",
                                "voice" => "语音",
                                _ => "文件",
                            };
                            format!("[{kind}] {}", media.name)
                        })
                        .unwrap_or_default()
                } else {
                    message.text
                };
                preview = if message.sender.is_empty() || body.is_empty() {
                    body
                } else {
                    format!("{}: {body}", message.sender)
                };
            }
            group["preview"] = preview.chars().take(160).collect::<String>().into();
            group["timestamp"] = timestamp.into();
            Ok(group)
        })
        .collect()
    }

    pub fn set_groups(&self, groups: &[Group]) -> Result<()> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM groups", [])?;
        for g in groups {
            tx.execute(
                "INSERT INTO groups VALUES(?1,?2)",
                params![g.id, serde_json::to_string(g)?],
            )?;
        }
        tx.commit()?;
        Ok(())
    }

    pub fn members(&self, group: &str) -> Result<Vec<Member>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row("SELECT body FROM members WHERE group_id=?", [group], |r| {
                r.get(0)
            })
            .optional()?;
        Ok(body
            .map(|s| serde_json::from_str(&s))
            .transpose()?
            .unwrap_or_default())
    }

    pub fn set_members(&self, group: &str, members: &[Member]) -> Result<()> {
        self.db.lock().unwrap().execute(
            "INSERT OR REPLACE INTO members VALUES(?1,?2)",
            params![group, serde_json::to_string(members)?],
        )?;
        Ok(())
    }

    pub fn media(&self, id: &str) -> Result<Media> {
        let conn = self.db.lock().unwrap();
        let body: String = conn
            .query_row("SELECT body FROM media WHERE id=?", [id], |r| r.get(0))
            .optional()?
            .context("附件不存在")?;
        Ok(serde_json::from_str(&body)?)
    }

    pub fn media_group(&self, id: &str) -> Result<String> {
        let conn = self.db.lock().unwrap();
        if let Some(group) = conn
            .query_row(
                "SELECT group_id FROM media_owners WHERE media_id=?1",
                [id],
                |r| r.get::<_, String>(0),
            )
            .optional()?
        {
            return Ok(group);
        }
        // Old media is readable only when its stored messages establish one owner.
        let mut stmt=conn.prepare("SELECT DISTINCT m.group_id FROM messages m,json_each(m.body,'$.media') AS a WHERE json_extract(a.value,'$.id')=?1 LIMIT 2")?;
        let groups = stmt
            .query_map([id], |r| r.get::<_, String>(0))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if groups.len() != 1 {
            bail!("附件没有唯一所属群，不能访问");
        }
        Ok(groups[0].clone())
    }

    pub fn media_for_group(&self, id: &str, group: &str) -> Result<Media> {
        if self.media_group(id)? != group {
            bail!("附件不属于该群");
        }
        self.media(id)
    }

    pub fn set_media(&self, media: &Media, group: &str) -> Result<()> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        tx.execute(
            "INSERT OR REPLACE INTO media VALUES(?1,?2)",
            params![media.id, serde_json::to_string(media)?],
        )?;
        tx.execute(
            "INSERT OR REPLACE INTO media_owners VALUES(?1,?2)",
            params![media.id, group],
        )?;
        tx.commit()?;
        Ok(())
    }

    pub fn message(&self, id: &str) -> Result<Option<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row("SELECT body FROM messages WHERE id=?", [id], |r| r.get(0))
            .optional()?;
        Ok(body.map(|s| serde_json::from_str(&s)).transpose()?)
    }

    pub fn source(&self, group: &str, source: &str) -> Result<Option<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row(
                "SELECT body FROM messages WHERE group_id=?1 AND source_id=?2",
                params![group, source],
                |r| r.get(0),
            )
            .optional()?;
        Ok(body.map(|s| serde_json::from_str(&s)).transpose()?)
    }

    pub fn insert_message(&self, message: &mut ChatMessage) -> Result<Option<Value>> {
        self.insert_message_kind(message, false)
    }

    pub fn insert_historical_message(&self, message: &mut ChatMessage) -> Result<Option<Value>> {
        self.insert_message_kind(message, true)
    }

    fn insert_message_kind(
        &self,
        message: &mut ChatMessage,
        historical: bool,
    ) -> Result<Option<Value>> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let existing: Option<String> = tx.query_row("SELECT body FROM messages WHERE id=?1 OR (source_id!='' AND group_id=?2 AND source_id=?3)",
            params![message.id, message.group, message.source_id], |r| r.get(0)).optional()?;
        if let Some(body) = existing {
            let old: ChatMessage = serde_json::from_str(&body)?;
            if old.id == message.id
                && (old.group != message.group
                    || old.text != message.text
                    || old.client_id != message.client_id
                    || old.access_client != message.access_client
                    || old.media.iter().map(|m| &m.id).collect::<Vec<_>>()
                        != message.media.iter().map(|m| &m.id).collect::<Vec<_>>()
                    || serde_json::to_value(&old.mentions)?
                        != serde_json::to_value(&message.mentions)?
                    || old.quote.as_ref().map(|q| &q.id) != message.quote.as_ref().map(|q| &q.id))
            {
                bail!("同一发送编号对应的内容发生变化");
            }
            *message = old;
            return Ok(None);
        }
        if message.outgoing && message.status == "queued" {
            if let Some(quote) = &message.quote {
                let source: Option<String> = tx
                    .query_row("SELECT body FROM messages WHERE id=?1", [&quote.id], |r| {
                        r.get(0)
                    })
                    .optional()?;
                let source: ChatMessage = serde_json::from_str(&source.context("引用消息不存在")?)?;
                if source.group != message.group || source.status == "recalled" {
                    bail!("引用消息不属于该群或已撤回");
                }
            }
            let pending: i64 = tx.query_row(
                "SELECT count(*) FROM messages WHERE status IN ('queued','sending')",
                [],
                |r| r.get(0),
            )?;
            if pending >= 100 {
                bail!("发送队列已满，请稍后再试");
            }
        }
        tx.execute(
            "INSERT INTO messages(id,group_id,source_id,body,status) VALUES(?1,?2,?3,?4,?5)",
            params![
                message.id,
                message.group,
                message.source_id,
                serde_json::to_string(message)?,
                message.status
            ],
        )?;
        message.seq = tx.last_insert_rowid();
        tx.execute(
            "UPDATE messages SET body=?1 WHERE id=?2",
            params![serde_json::to_string(message)?, message.id],
        )?;
        let mut event = Self::append_event(&tx, message, true)?;
        if historical {
            event["historical"] = json!(true);
            tx.execute(
                "UPDATE events SET body=?1 WHERE seq=?2",
                params![event.to_string(), event["seq"].as_i64().unwrap()],
            )?;
        }
        if !message.source_id.is_empty() {
            tx.execute(
                "DELETE FROM history_sources WHERE group_id=?1 AND source_id=?2",
                params![message.group, message.source_id],
            )?;
        }
        tx.commit()?;
        Ok(Some(event))
    }

    pub fn enrich_native_quote(
        &self,
        id: &str,
        text: &str,
        quote: &Quote,
        historical: bool,
    ) -> Result<Option<Value>> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let body: Option<String> = tx
            .query_row("SELECT body FROM messages WHERE id=?1", [id], |row| {
                row.get(0)
            })
            .optional()?;
        let Some(body) = body else {
            return Ok(None);
        };
        let mut message: ChatMessage = serde_json::from_str(&body)?;
        if message.status == "recalled"
            || !message.recall_status.is_empty()
            || !message.client_id.is_empty()
            || message.quote.is_some()
        {
            return Ok(None);
        }
        let mut quote = quote.clone();
        let source: Option<String> = tx
            .query_row(
                "SELECT body FROM messages WHERE id=?1",
                [&quote.id],
                |row| row.get(0),
            )
            .optional()?;
        if let Some(source) = source {
            let source: ChatMessage = serde_json::from_str(&source)?;
            if source.group != message.group || source.status == "recalled" {
                quote.unavailable = true;
                quote.text.clear();
            }
        }
        message.text = text.to_owned();
        message.quote = Some(quote);
        tx.execute(
            "UPDATE messages SET body=?1 WHERE id=?2",
            params![serde_json::to_string(&message)?, id],
        )?;
        let mut event = Self::append_event(&tx, &message, false)?;
        if historical {
            event["historical"] = json!(true);
            tx.execute(
                "UPDATE events SET body=?1 WHERE seq=?2",
                params![event.to_string(), event["seq"].as_i64().unwrap()],
            )?;
        }
        tx.commit()?;
        Ok(Some(event))
    }

    pub fn update_message(&self, message: &ChatMessage) -> Result<Value> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let body: String = tx.query_row(
            "SELECT body FROM messages WHERE id=?1",
            [&message.id],
            |r| r.get(0),
        )?;
        let current: ChatMessage = serde_json::from_str(&body)?;
        let mut message = message.clone();
        // A late send acknowledgment must not restore a withdrawn quote or message.
        if current.status == "recalled" {
            message = current;
        } else {
            message.quote = current.quote;
            message.recall_status = current.recall_status;
            message.recall_error = current.recall_error;
            message.recall_client = current.recall_client;
            message.access_client = current.access_client;
        }
        tx.execute(
            "UPDATE messages SET body=?1,status=?2,source_id=?3 WHERE id=?4",
            params![
                serde_json::to_string(&message)?,
                message.status,
                message.source_id,
                message.id
            ],
        )?;
        for source in std::iter::once(&message.source_id)
            .chain(message.source_ids.iter())
            .filter(|source| !source.is_empty())
        {
            tx.execute(
                "DELETE FROM history_sources WHERE group_id=?1 AND source_id=?2",
                params![message.group, source],
            )?;
        }
        let event = Self::append_event(&tx, &message, false)?;
        tx.commit()?;
        Ok(event)
    }

    pub fn quote(&self, group: &str, requested: &Quote) -> Result<Quote> {
        ensure_quote_id(&requested.id)?;
        let source = self.message(&requested.id)?.context("引用消息不存在")?;
        if source.group != group || source.status == "recalled" {
            bail!("引用消息不属于该群或已撤回");
        }
        let text = if source.text.is_empty() {
            source
                .media
                .iter()
                .map(|m| {
                    format!(
                        "[{}] {}",
                        match m.kind.as_str() {
                            "image" => "图片",
                            "video" => "视频",
                            "gif" => "GIF",
                            "voice" => "语音",
                            _ => "文件",
                        },
                        m.name
                    )
                })
                .collect::<Vec<_>>()
                .join(" ")
        } else {
            source.text
        };
        if (!requested.sender.is_empty() && requested.sender != source.sender)
            || (!requested.text.is_empty() && requested.text != text)
            || requested.unavailable
        {
            bail!("引用消息快照不匹配，请重新选择原消息");
        }
        Ok(Quote {
            id: source.id,
            sender: source.sender,
            text,
            unavailable: false,
        })
    }

    #[cfg(test)]
    pub fn begin_recall(&self, id: &str, now: i64) -> Result<(ChatMessage, Option<Value>)> {
        self.begin_recall_as(id, now, "")
    }

    pub fn begin_recall_as(
        &self,
        id: &str,
        now: i64,
        client: &str,
    ) -> Result<(ChatMessage, Option<Value>)> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let body: String = tx
            .query_row("SELECT body FROM messages WHERE id=?1", [id], |r| r.get(0))
            .optional()?
            .context("消息不存在")?;
        let mut message: ChatMessage = serde_json::from_str(&body)?;
        if message.status == "recalled" || !message.recall_status.is_empty() {
            return Ok((message, None));
        }
        check_recall(&message, now)?;
        let pending: i64 = tx.query_row(
            "SELECT count(*) FROM messages WHERE json_extract(body,'$.recall_status')='pending'",
            [],
            |r| r.get(0),
        )?;
        if pending >= 100 {
            bail!("待确认撤回过多，请稍后再试");
        }
        message.recall_status = "pending".into();
        message.recall_client = client.to_owned();
        message.recall_error.clear();
        tx.execute(
            "UPDATE messages SET body=?1 WHERE id=?2",
            params![serde_json::to_string(&message)?, message.id],
        )?;
        let event = Self::append_event(&tx, &message, false)?;
        tx.commit()?;
        Ok((message, Some(event)))
    }

    pub fn pending_recalls(&self) -> Result<Vec<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let mut stmt=conn.prepare("SELECT body FROM messages WHERE json_extract(body,'$.recall_status')='pending' ORDER BY seq LIMIT 100")?;
        stmt.query_map([], |r| r.get::<_, String>(0))?
            .map(|body| Ok(serde_json::from_str(&body?)?))
            .collect()
    }

    pub fn finish_recall(&self, id: &str, status: &str, error: &str) -> Result<Vec<Value>> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        let body: Option<String> = tx
            .query_row("SELECT body FROM messages WHERE id=?1", [id], |r| r.get(0))
            .optional()?;
        let Some(body) = body else {
            return Ok(Vec::new());
        };
        let mut message: ChatMessage = serde_json::from_str(&body)?;
        if message.status == "recalled"
            || !["pending", "uncertain"].contains(&message.recall_status.as_str())
        {
            return Ok(Vec::new());
        }
        if status == "recalled" {
            message.status = "recalled".into();
            message.text.clear();
            message.media.clear();
            message.mentions.clear();
            message.quote = None;
            message.recall_status.clear();
            message.recall_error.clear();
        } else {
            message.recall_status = if status == "failed" {
                "failed"
            } else {
                "uncertain"
            }
            .into();
            message.recall_error = error.chars().take(1000).collect();
        }
        tx.execute(
            "UPDATE messages SET body=?1,status=?2 WHERE id=?3",
            params![serde_json::to_string(&message)?, message.status, message.id],
        )?;
        let mut events = vec![Self::append_event(&tx, &message, false)?];
        if status == "recalled" {
            let references = {
                let mut stmt = tx.prepare(
                    "SELECT body FROM messages WHERE json_extract(body,'$.quote.id')=?1",
                )?;
                stmt.query_map([id], |r| r.get::<_, String>(0))?
                    .collect::<rusqlite::Result<Vec<_>>>()?
            };
            for body in references {
                let mut quoted: ChatMessage = serde_json::from_str(&body)?;
                if let Some(quote) = &mut quoted.quote {
                    quote.unavailable = true;
                    quote.text.clear();
                }
                tx.execute(
                    "UPDATE messages SET body=?1 WHERE id=?2",
                    params![serde_json::to_string(&quoted)?, quoted.id],
                )?;
                events.push(Self::append_event(&tx, &quoted, false)?);
            }
        }
        tx.commit()?;
        Ok(events)
    }

    fn append_event(
        tx: &rusqlite::Transaction<'_>,
        message: &ChatMessage,
        created: bool,
    ) -> Result<Value> {
        // Clients distinguish a new arrival from a receipt, recall or metadata update without comparing clocks.
        let mut event = json!({"type":"message", "message":message, "created":created});
        tx.execute("INSERT INTO events(body) VALUES(?)", [event.to_string()])?;
        event["seq"] = tx.last_insert_rowid().into();
        tx.execute(
            "UPDATE events SET body=?1 WHERE seq=?2",
            params![event.to_string(), tx.last_insert_rowid()],
        )?;
        Ok(event)
    }

    pub fn history(&self, group: &str, before: i64, search: &str) -> Result<Vec<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let time = if before > 0 {
            conn.query_row("SELECT CAST(json_extract(body,'$.timestamp') AS INTEGER) FROM messages WHERE group_id=?1 AND seq=?2",params![group,before],|r|r.get::<_,i64>(0)).optional()?.context("历史游标不属于该群")?
        } else {
            i64::MAX
        };
        let mut stmt = conn.prepare(
            "SELECT body FROM messages WHERE group_id=?1 AND
             (CAST(json_extract(body,'$.timestamp') AS INTEGER),seq)<(?3,?2) AND
             (?4='' OR instr(json_extract(body,'$.text'),?4)>0
                    OR instr(json_extract(body,'$.sender'),?4)>0
                    OR EXISTS(SELECT 1 FROM json_each(messages.body,'$.media')
                              WHERE instr(json_extract(value,'$.name'),?4)>0))
             ORDER BY CAST(json_extract(body,'$.timestamp') AS INTEGER) DESC,seq DESC LIMIT 60",
        )?;
        let rows = stmt.query_map(
            params![
                group,
                if before > 0 { before } else { i64::MAX },
                time,
                search
            ],
            |r| r.get::<_, String>(0),
        )?;
        let mut result: Vec<ChatMessage> = rows
            .map(|s| Ok(serde_json::from_str(&s?)?))
            .collect::<Result<_>>()?;
        result.reverse();
        Ok(result)
    }

    pub fn history_progress(&self, group: &str) -> Result<crate::history::Progress> {
        let conn = self.db.lock().unwrap();
        let key = format!("history_v1:{group}");
        let body: Option<String> = conn
            .query_row("SELECT body FROM state WHERE key=?1", [key], |row| {
                row.get(0)
            })
            .optional()?;
        let mut progress: crate::history::Progress = body
            .map(|body| serde_json::from_str(&body))
            .transpose()?
            .unwrap_or_default();
        progress.pending=conn.query_row("SELECT EXISTS(SELECT 1 FROM history_sources h WHERE h.group_id=?1 AND NOT EXISTS(SELECT 1 FROM messages m WHERE m.group_id=h.group_id AND m.source_id=h.source_id))",[group],|row|row.get(0))?;
        // Cached quote originals can be arbitrarily old: only a confirmed native
        // page advances this contiguous scan boundary, never an unrelated cached row.
        Ok(progress)
    }

    pub fn set_history_progress(
        &self,
        group: &str,
        progress: &crate::history::Progress,
    ) -> Result<()> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        tx.execute(
            "INSERT OR REPLACE INTO state(key,body) VALUES(?1,?2)",
            params![
                format!("history_v1:{group}"),
                serde_json::to_string(progress)?
            ],
        )?;
        for source in &progress.sources {
            tx.execute("INSERT OR IGNORE INTO history_sources(group_id,source_id) SELECT ?1,?2 WHERE NOT EXISTS(SELECT 1 FROM messages WHERE group_id=?1 AND source_id=?2)",params![group,source])?;
        }
        tx.commit()?;
        Ok(())
    }

    pub fn history_with_imports(&self, group: &str, before: i64) -> Result<Vec<ChatMessage>> {
        let mut rows = self.history(group, before, "")?;
        if before > 0 {
            let anchor = self
                .message_by_sequence(group, before)?
                .context("历史游标不属于该群")?;
            // A native batch can fill a sparse gap above a cached quote original,
            // or share an epoch second with an older relay seq. Include it explicitly
            // so it reaches the requesting UI even when WS history inserts are suppressed.
            for source in self.history_progress(group)?.sources {
                if let Some(message) = self.source(group, &source)?
                    && (message.timestamp, message.seq) > (anchor.timestamp, before)
                    && !rows.iter().any(|row| row.id == message.id)
                {
                    rows.push(message);
                }
            }
            rows.sort_by_key(|message| (message.timestamp, message.seq));
        }
        Ok(rows)
    }

    fn message_by_sequence(&self, group: &str, sequence: i64) -> Result<Option<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row(
                "SELECT body FROM messages WHERE group_id=?1 AND seq=?2",
                params![group, sequence],
                |row| row.get(0),
            )
            .optional()?;
        Ok(body.map(|body| serde_json::from_str(&body)).transpose()?)
    }

    pub fn events(&self, after: i64) -> Result<Vec<Value>> {
        let conn = self.db.lock().unwrap();
        let mut stmt =
            conn.prepare("SELECT e.body,(SELECT m.body FROM messages m WHERE m.id=json_extract(e.body,'$.message.id') AND (m.status='recalled' OR json_extract(m.body,'$.quote.unavailable')=1)) FROM events e WHERE e.seq>? ORDER BY e.seq LIMIT 256")?;
        stmt.query_map([after], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?))
        })?
        .map(|row| {
            let (body, current) = row?;
            let mut event: Value = serde_json::from_str(&body)?;
            if let Some(current) = current {
                event["message"] = serde_json::from_str(&current)?;
            }
            Ok(event)
        })
        .collect()
    }

    pub fn event_head(&self) -> Result<i64> {
        Ok(self.db.lock().unwrap().query_row(
            "SELECT coalesce(max(seq),0) FROM events",
            [],
            |r| r.get(0),
        )?)
    }

    pub fn next_queued(&self) -> Result<Option<ChatMessage>> {
        let conn = self.db.lock().unwrap();
        let body: Option<String> = conn
            .query_row(
                "SELECT body FROM messages WHERE status='queued' ORDER BY seq LIMIT 1",
                [],
                |r| r.get(0),
            )
            .optional()?;
        Ok(body.map(|s| serde_json::from_str(&s)).transpose()?)
    }

    pub fn remove_media(&self, id: &str) -> Result<()> {
        let mut conn = self.db.lock().unwrap();
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM media WHERE id=?", [id])?;
        tx.execute("DELETE FROM media_owners WHERE media_id=?", [id])?;
        tx.commit()?;
        Ok(())
    }
}

fn ensure_quote_id(id: &str) -> Result<()> {
    if id.is_empty() || id.len() > 160 || id.contains('\0') {
        bail!("引用消息编号无效");
    }
    Ok(())
}
pub fn check_recall(message: &ChatMessage, now: i64) -> Result<()> {
    if !message.outgoing
        || !["sent", "received"].contains(&message.status.as_str())
        || (message.source_id.is_empty() && message.source_ids.is_empty())
    {
        bail!("只能撤回已经微信确认的本人消息");
    }
    let age = now.checked_sub(message.timestamp).context("消息时间无效")?;
    if !(0..180_000).contains(&age) {
        bail!("消息已超过三分钟撤回时限或时间无效");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn message(id: &str) -> ChatMessage {
        serde_json::from_value(json!({"id":id,"group":"g","sender":"小林","text":"你好","timestamp":0,"status":"queued"})).unwrap()
    }
    #[test]
    fn native_history_progress_handles_sparse_quotes_pending_media_recall_and_same_second_imports()
    {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("history.sqlite");
        let store = Store::open(&path).unwrap();
        let mut anchor = message("anchor");
        anchor.timestamp = 1000;
        anchor.source_id = "wx:g:message__message_0.db:100:100".into();
        store.insert_message(&mut anchor).unwrap();
        let mut sparse = message("old-quote-original");
        sparse.timestamp = 1;
        sparse.source_id = "wx:g:message__message_0.db:1:1".into();
        store.insert_message(&mut sparse).unwrap();
        let plan = store.history_progress("g").unwrap();
        assert!(!plan.initialized && plan.before_source.is_empty());
        let native = "wx:g:message__message_0.db:90:90";
        let progress = crate::history::Progress {
            initialized: true,
            before_source: native.into(),
            sources: vec![native.into()],
            exhausted: true,
            ..Default::default()
        };
        store.set_history_progress("g", &progress).unwrap();
        assert!(store.history_progress("g").unwrap().pending);
        assert_eq!(store.history_progress("g").unwrap().before_source, native);
        let mut imported = message("imported-same-second");
        imported.source_id = native.into();
        imported.timestamp = 1000;
        let event = store
            .insert_historical_message(&mut imported)
            .unwrap()
            .unwrap();
        assert_eq!(event["historical"], true);
        assert!(!store.history_progress("g").unwrap().pending);
        assert!(
            !store
                .history("g", anchor.seq, "")
                .unwrap()
                .iter()
                .any(|row| row.id == imported.id)
        );
        assert!(
            store
                .history_with_imports("g", anchor.seq)
                .unwrap()
                .iter()
                .any(|row| row.id == imported.id)
        );
        assert!(
            store
                .history_with_imports("g", sparse.seq)
                .unwrap()
                .iter()
                .any(|row| row.id == imported.id)
        );
        imported.status = "recalled".into();
        imported.text.clear();
        store.update_message(&imported).unwrap();
        let replay = store.events(0).unwrap();
        let old = replay
            .iter()
            .find(|event| event["historical"] == true)
            .unwrap();
        assert_eq!(old["message"]["status"], "recalled");
        assert_eq!(old["message"]["text"], "");
        drop(store);
        let reopened = Store::open(&path).unwrap();
        let progress = reopened.history_progress("g").unwrap();
        assert!(progress.exhausted && !progress.pending);
        assert_eq!(progress.before_source, native);
        assert!(reopened.history_with_imports("other", anchor.seq).is_err());
        let alias = "wx:g:message__message_1.db:120:120";
        reopened
            .set_history_progress(
                "g",
                &crate::history::Progress {
                    initialized: true,
                    before_source: alias.into(),
                    sources: vec![alias.into()],
                    exhausted: false,
                    ..Default::default()
                },
            )
            .unwrap();
        assert!(reopened.history_progress("g").unwrap().pending);
        let mut aggregate = message("aggregate");
        aggregate.status = "queued".into();
        aggregate.outgoing = true;
        reopened.insert_message(&mut aggregate).unwrap();
        aggregate.status = "sent".into();
        aggregate.source_id = "wx:g:message__message_0.db:119:119".into();
        aggregate.source_ids = vec![aggregate.source_id.clone(), alias.into()];
        reopened.update_message(&aggregate).unwrap();
        assert!(!reopened.history_progress("g").unwrap().pending);
    }

    #[test]
    fn dedup_history_and_replay() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("s.db")).unwrap();
        let mut m = message("a");
        let inserted = store.insert_message(&mut m).unwrap().unwrap();
        assert_eq!(inserted["seq"], 1);
        assert_eq!(inserted["created"], true);
        assert!(store.insert_message(&mut m).unwrap().is_none());
        m.status = "sent".into();
        let updated = store.update_message(&m).unwrap();
        assert_eq!(updated["seq"], 2);
        assert_eq!(updated["created"], false);
        let replay = store.events(0).unwrap();
        assert_eq!(replay.len(), 2);
        assert_eq!(replay[0]["created"], true);
        assert_eq!(replay[1]["created"], false);
        assert_eq!(store.history("g", 0, "你好").unwrap().len(), 1);
        m.text = "changed".into();
        assert!(store.insert_message(&mut m).is_err());
    }
    #[test]
    fn crash_does_not_resend_submitted_messages() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("s.db");
        {
            let store = Store::open(&path).unwrap();
            let mut m = message("a");
            m.status = "sending".into();
            store.insert_message(&mut m).unwrap();
        }
        let store = Store::open(&path).unwrap();
        assert_eq!(store.message("a").unwrap().unwrap().status, "uncertain");
        assert!(store.next_queued().unwrap().is_none());
    }

    #[test]
    fn search_matches_only_visible_content() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("s.db")).unwrap();
        let mut m = message("secret-internal-id");
        m.media.push(Media {
            id: "hidden-media-id".into(),
            name: "今日设计.gif".into(),
            kind: "gif".into(),
            size: 1,
            sha256: "hidden-hash".into(),
            path: "/private/hidden-directory/a.gif".into(),
        });
        store.insert_message(&mut m).unwrap();
        for query in ["你好", "小林", "设计", "今日设计.gif"] {
            assert_eq!(store.history("g", 0, query).unwrap().len(), 1, "{query}");
        }
        for query in [
            "secret-internal-id",
            "hidden-media-id",
            "hidden-hash",
            "hidden-directory",
            "queued",
            "timestamp",
        ] {
            assert!(store.history("g", 0, query).unwrap().is_empty(), "{query}");
        }
        assert_eq!(store.event_head().unwrap(), 1);
    }

    #[test]
    fn concurrent_retries_insert_and_enqueue_once() {
        let temp = tempfile::tempdir().unwrap();
        let store = std::sync::Arc::new(Store::open(&temp.path().join("s.db")).unwrap());
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(16));
        let workers: Vec<_> = (0..16)
            .map(|_| {
                let store = store.clone();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    let mut m = message("same-id");
                    m.outgoing = true;
                    store.insert_message(&mut m).unwrap().is_some()
                })
            })
            .collect();
        assert_eq!(
            workers
                .into_iter()
                .map(|w| w.join().unwrap() as usize)
                .sum::<usize>(),
            1
        );
        assert_eq!(store.events(0).unwrap().len(), 1);
        assert_eq!(store.next_queued().unwrap().unwrap().id, "same-id");
    }

    #[test]
    fn queue_is_bounded_and_pending_messages_survive_restart() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("s.db");
        {
            let store = Store::open(&path).unwrap();
            for index in 0..100 {
                let mut m = message(&index.to_string());
                m.outgoing = true;
                store.insert_message(&mut m).unwrap();
            }
            let mut retry = message("0");
            retry.outgoing = true;
            assert!(store.insert_message(&mut retry).unwrap().is_none());
            let mut extra = message("overflow");
            extra.outgoing = true;
            assert!(store.insert_message(&mut extra).is_err());
            let mut first = store.next_queued().unwrap().unwrap();
            first.status = "sending".into();
            store.update_message(&first).unwrap();
        }
        let store = Store::open(&path).unwrap();
        assert_eq!(store.message("0").unwrap().unwrap().status, "uncertain");
        assert_eq!(store.next_queued().unwrap().unwrap().id, "1");
    }

    #[test]
    fn group_summaries_use_latest_persisted_message_and_leave_unsynced_groups_empty() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("s.db");
        {
            let store = Store::open(&path).unwrap();
            store
                .set_groups(&[
                    Group {
                        id: "g".into(),
                        name: "已同步群".into(),
                        count: 2,
                        avatar: "c21hbGwtanBlZw==".into(),
                    },
                    Group {
                        id: "empty".into(),
                        name: "尚未同步群".into(),
                        count: 4,
                        avatar: String::new(),
                    },
                ])
                .unwrap();
            let mut first = message("first");
            first.timestamp = 5000;
            store.insert_message(&mut first).unwrap();
            let mut latest = message("latest");
            latest.text.clear();
            latest.timestamp = 6000;
            latest.media.push(Media {
                id: "clip".into(),
                name: "开场.mov".into(),
                kind: "video".into(),
                size: 10,
                sha256: String::new(),
                path: "/private/clip.mov".into(),
            });
            store.insert_message(&mut latest).unwrap();
            let mut old = message("late-old-history");
            old.timestamp = 1234;
            store.insert_message(&mut old).unwrap();
        }
        let store = Store::open(&path).unwrap();
        let groups = store.group_summaries().unwrap();
        assert_eq!(groups[0]["preview"], "小林: [视频] 开场.mov");
        assert_eq!(groups[0]["timestamp"], 6000);
        assert_eq!(groups[0]["avatar"], "c21hbGwtanBlZw==");
        assert_eq!(groups[1]["preview"], "");
        assert_eq!(groups[1]["timestamp"], 0);
        assert!(groups[1].get("avatar").is_none());
    }

    #[test]
    fn forwarding_defaults_empty_persists_and_cancels_only_unsent_messages() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("s.db");
        let store = Store::open(&path).unwrap();
        assert!(store.forward_groups().unwrap().is_none());
        store.initialize_forward_groups(&BTreeSet::new()).unwrap();
        store
            .initialize_forward_groups(&BTreeSet::from(["g".into()]))
            .unwrap();
        assert!(store.forward_groups().unwrap().unwrap().is_empty());
        store
            .set_groups(&[
                Group {
                    id: "g".into(),
                    name: "G".into(),
                    count: 1,
                    avatar: String::new(),
                },
                Group {
                    id: "h".into(),
                    name: "H".into(),
                    count: 1,
                    avatar: String::new(),
                },
            ])
            .unwrap();
        let selected = BTreeSet::from(["g".into()]);
        store.set_forward_groups(&selected).unwrap();
        for (id, group, status) in [
            ("allowed", "g", "queued"),
            ("cancelled", "h", "queued"),
            ("submitted", "h", "sending"),
        ] {
            let mut m = message(id);
            m.group = group.into();
            m.status = status.into();
            store.insert_message(&mut m).unwrap();
        }
        let events = store.set_forward_groups(&selected).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(
            store.message("cancelled").unwrap().unwrap().status,
            "failed"
        );
        assert_eq!(
            store.message("submitted").unwrap().unwrap().status,
            "sending"
        );
        assert_eq!(store.message("allowed").unwrap().unwrap().status, "queued");
        assert!(
            store
                .set_forward_groups(&BTreeSet::from(["unknown".into()]))
                .is_err()
        );
        assert_eq!(store.forward_groups().unwrap().unwrap(), selected);
        drop(store);
        assert_eq!(
            Store::open(&path)
                .unwrap()
                .forward_groups()
                .unwrap()
                .unwrap(),
            selected
        );
    }

    #[test]
    fn media_ownership_is_bound_and_legacy_attachments_require_one_message_group() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("s.db")).unwrap();
        let media = Media {
            id: "file".into(),
            name: "a.txt".into(),
            kind: "file".into(),
            size: 1,
            sha256: String::new(),
            path: "/private/a.txt".into(),
        };
        store.set_media(&media, "g").unwrap();
        assert!(store.media_for_group("file", "g").is_ok());
        assert!(store.media_for_group("file", "h").is_err());
        store
            .db
            .lock()
            .unwrap()
            .execute("DELETE FROM media_owners", [])
            .unwrap();
        assert!(store.media_group("file").is_err());
        let mut first = message("first");
        first.media.push(media.clone());
        store.insert_message(&mut first).unwrap();
        assert_eq!(store.media_group("file").unwrap(), "g");
        let mut second = message("second");
        second.group = "h".into();
        second.media.push(media);
        store.insert_message(&mut second).unwrap();
        assert!(store.media_group("file").is_err());
    }

    #[test]
    fn late_history_and_equal_timestamps_page_without_gaps_or_duplicates() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::open(&temp.path().join("s.db")).unwrap();
        let mut expected = Vec::new();
        for index in (0..137).rev() {
            let mut m = message(&index.to_string());
            m.timestamp = index / 3;
            store.insert_message(&mut m).unwrap();
            expected.push(m);
        }
        expected.sort_by_key(|m| (m.timestamp, m.seq));
        let mut all = Vec::new();
        let mut cursor = 0;
        loop {
            let page = store.history("g", cursor, "").unwrap();
            if page.is_empty() {
                break;
            }
            cursor = page[0].seq;
            all.splice(0..0, page);
        }
        assert_eq!(
            all.iter().map(|m| &m.id).collect::<Vec<_>>(),
            expected.iter().map(|m| &m.id).collect::<Vec<_>>()
        );
        assert!(store.history("other", cursor, "").is_err());
    }

    #[test]
    fn quotes_are_canonical_and_recall_redacts_source_references_and_old_events() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("s.db");
        let store = Store::open(&path).unwrap();
        let mut source = message("source");
        source.outgoing = true;
        source.status = "sent".into();
        source.timestamp = 1000;
        source.source_id = "native-one".into();
        source.source_ids = vec!["native-one".into(), "native-two".into()];
        store.insert_message(&mut source).unwrap();
        let request = Quote {
            id: "source".into(),
            sender: String::new(),
            text: String::new(),
            unavailable: false,
        };
        let quote = store.quote("g", &request).unwrap();
        assert_eq!(quote.sender, "小林");
        assert_eq!(quote.text, "你好");
        assert!(store.quote("other", &request).is_err());
        let mut forged = request.clone();
        forged.text = "伪造内容".into();
        assert!(store.quote("g", &forged).is_err());
        let mut reply = message("reply");
        reply.quote = Some(quote);
        reply.text = "回答".into();
        store.insert_message(&mut reply).unwrap();
        let stale_source = source.clone();
        let mut stale_reply = reply.clone();
        stale_reply.status = "sent".into();
        assert!(store.begin_recall("source", 1001).unwrap().1.is_some());
        assert!(store.begin_recall("source", 1002).unwrap().1.is_none());
        drop(store);
        let store = Store::open(&path).unwrap();
        assert_eq!(store.pending_recalls().unwrap().len(), 1);
        assert_eq!(
            store
                .finish_recall("source", "uncertain", "等待确认")
                .unwrap()
                .len(),
            1
        );
        assert_eq!(store.message("source").unwrap().unwrap().text, "你好");
        assert_eq!(
            store.finish_recall("source", "recalled", "").unwrap().len(),
            2
        );
        let late_reply = store.update_message(&stale_reply).unwrap();
        assert_eq!(late_reply["message"]["quote"]["unavailable"], true);
        assert_eq!(late_reply["message"]["quote"]["text"], "");
        assert_eq!(
            store.update_message(&stale_source).unwrap()["message"]["status"],
            "recalled"
        );
        let source = store.message("source").unwrap().unwrap();
        assert_eq!(source.status, "recalled");
        assert!(source.text.is_empty());
        assert_eq!(source.source_ids.len(), 2);
        let reply = store.message("reply").unwrap().unwrap();
        assert_eq!(reply.text, "回答");
        assert!(reply.quote.as_ref().unwrap().unavailable);
        assert!(reply.quote.unwrap().text.is_empty());
        assert!(store.quote("g", &request).is_err());
        for event in store.events(0).unwrap() {
            let m = &event["message"];
            if m["id"] == "source" {
                assert_eq!(m["text"], "");
                assert_eq!(m["status"], "recalled");
            }
            if m["id"] == "reply" {
                assert_eq!(m["quote"]["text"], "");
            }
        }
    }

    #[test]
    fn recall_checks_owner_confirmation_and_exact_time_window() {
        let mut m = message("own");
        m.timestamp = 1000;
        m.status = "sent".into();
        m.outgoing = true;
        m.source_id = "source".into();
        assert!(check_recall(&m, 1000).is_ok());
        assert!(check_recall(&m, 180999).is_ok());
        assert!(check_recall(&m, 181000).is_err());
        assert!(check_recall(&m, 181001).is_err());
        assert!(check_recall(&m, 999).is_err());
        m.outgoing = false;
        assert!(check_recall(&m, 1001).is_err());
        m.outgoing = true;
        m.source_id.clear();
        assert!(check_recall(&m, 1001).is_err());
        m.source_ids = vec!["source".into()];
        assert!(check_recall(&m, 1001).is_ok());
        m.status = "uncertain".into();
        assert!(check_recall(&m, 1001).is_err());
    }
}
