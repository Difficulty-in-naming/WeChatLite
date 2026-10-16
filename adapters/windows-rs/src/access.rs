use crate::actions::Actions;
use crate::{
    db::Db,
    engine::{Backend, Rejected, Step, source, system_message, text, xml, xml_text},
    media::{Media, avatar_data},
    native::{NativeClient, NativeHttpError},
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    borrow::Cow,
    collections::{BTreeSet, HashMap, VecDeque},
    io::Read,
    path::{Path, PathBuf},
};
use uuid::Uuid;

pub struct Local {
    pub db: Db,
    native: NativeClient,
    media: Media,
    allowed: BTreeSet<String>,
    directory: PathBuf,
    avatars: HashMap<String, String>,
    avatar_order: VecDeque<String>,
    actions: Option<Actions>,
    actions_error: String,
    quotes: Option<crate::quotes::Quotes>,
    quotes_error: String,
}
impl Local {
    pub fn connect(directory: PathBuf) -> Result<Self> {
        let mut native = NativeClient::new(directory.join("native"));
        native.restrict(&[])?;
        crate::native::verify_wechat()?;
        let db = Db::connect(&directory.join("wechat"))?;
        let (actions, actions_error) = match Actions::new(directory.clone(), &db.wxid) {
            Ok(actions) => (Some(actions), String::new()),
            Err(error) => (None, error.to_string()),
        };
        let (quotes, quotes_error) = match crate::quotes::Quotes::new(directory.clone(), &db.wxid) {
            Ok(quotes) => (Some(quotes), String::new()),
            Err(error) => (None, error.to_string()),
        };
        Ok(Self {
            db,
            native,
            media: Media::new(directory.clone()),
            allowed: BTreeSet::new(),
            directory,
            avatars: HashMap::new(),
            avatar_order: VecDeque::new(),
            actions,
            actions_error,
            quotes,
            quotes_error,
        })
    }
    fn avatar(&mut self, id: &str) -> String {
        if id.is_empty() {
            return String::new();
        }
        if let Some(value) = self.avatars.get(id) {
            return value.clone();
        }
        let result = self
            .db
            .avatar(id)
            .ok()
            .flatten()
            .map(|bytes| avatar_data(&bytes))
            .unwrap_or_default();
        if self.avatars.len() == 512 {
            if let Some(old) = self.avatar_order.pop_front() {
                self.avatars.remove(&old);
            }
        }
        self.avatar_order.push_back(id.to_owned());
        self.avatars.insert(id.to_owned(), result.clone());
        result
    }
    fn identity(&mut self, group: &str, row: &Value) -> Result<(String, String)> {
        message_identity(group, row, &self.db.wxid.clone(), |prefix| {
            self.db.display_name(group, prefix)
        })
    }
    fn referenced_original(&mut self, group: &str, reference: &Value) -> Result<Option<Value>> {
        use md5::{Digest, Md5};
        if reference["type"] != 1 && reference["type"] != 3 {
            return Ok(None);
        }
        let server = text(reference, "server_id")
            .parse::<u64>()
            .context("引用来源编号无效")?;
        let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
        let mut original = None;
        for rel in self.db.rels().into_iter().filter(|rel| {
            rel.strip_prefix("message/message_")
                .and_then(|v| v.strip_suffix(".db"))
                .is_some_and(|v| !v.is_empty() && v.bytes().all(|c| c.is_ascii_digit()))
        }) {
            let connection = self.db.open(&rel)?;
            if !connection.query_row(
                "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1)",
                [&table],
                |row| row.get::<_, bool>(0),
            )? {
                continue;
            }
            let found = connection
                .prepare(&format!(
                    "SELECT local_id,sort_seq FROM {table} WHERE server_id=?1 LIMIT 2"
                ))?
                .query_map([server as i64], |row| {
                    Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            if found.len() > 1 || (!found.is_empty() && original.is_some()) {
                return Ok(None); // A duplicate server ID must never select whichever shard was visited first.
            }
            if let Some((id, sequence)) = found.first().copied() {
                let locator =
                    json!({"local_id":id,"sort_seq":sequence,"shard":rel.replace('/',"__")});
                let row = self.db.reload(group, &locator)?;
                let raw = self.db.raw_message(group, id, text(&locator, "shard"))?;
                if !crate::quotes::matches_reference(group, &row, &raw, &self.db.wxid, reference) {
                    return Ok(None);
                }
                original = Some(row);
            }
        }
        Ok(original)
    }
}
impl Drop for Local {
    fn drop(&mut self) {
        let _ = self.native.restrict(&[]);
        self.db.release();
    }
}
impl Backend for Local {
    fn self_id(&self) -> String {
        self.db.wxid.clone()
    }
    fn catalog(&mut self) -> Result<Vec<Value>> {
        let groups = self.db.groups()?;
        Ok(groups.into_iter().map(|group| {
            let id=group["id"].as_str().or_else(||group["username"].as_str()).unwrap_or_default();
            json!({"id":id,"name":text(&group,"name"),"count":group["member_count"].as_u64().unwrap_or(0),"avatar":self.avatar(id)})
        }).collect())
    }
    fn members(&mut self, group: &str) -> Result<Vec<Value>> {
        self.db.members(group)?.into_iter().map(|member| {
            let id=text(&member,"username");
            let name=self.db.display_name(group,id)?;
            Ok(json!({"id":id,"name":if name.is_empty(){"群成员"}else{&name},"remark":text(&member,"remark"),"avatar":self.avatar(id)}))
        }).collect()
    }
    fn announcement(&mut self, group: &str) -> Result<Value> {
        let mut announcement = self.db.announcement(group)?;
        let editor = text(&announcement, "author_id").to_owned();
        if announcement["available"] == true && !editor.is_empty() {
            let name = self.db.display_name(group, &editor).unwrap_or_default();
            if name != editor {
                announcement["author"] = json!(name);
            }
            announcement["avatar"] = json!(self.avatar(&editor));
        }
        Ok(announcement)
    }
    fn recent(&mut self, group: &str, limit: usize) -> Result<Vec<Value>> {
        self.db.recent(group, limit)
    }
    fn page(&mut self, group: &str, position: &Value, limit: usize) -> Result<Vec<Value>> {
        self.db.page(group, position, limit)
    }
    fn older(&mut self, group: &str, before: Option<&Value>, limit: usize) -> Result<Vec<Value>> {
        self.db.older(group, before, limit)
    }
    fn reload(&mut self, group: &str, locator: &Value) -> Result<Value> {
        self.db.reload(group, locator)
    }
    fn signatures(&mut self) -> Result<Value> {
        Ok(serde_json::to_value(self.db.signatures()?)?)
    }
    fn authorize(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        if groups == &self.allowed {
            return Ok(());
        }
        let list: Vec<String> = groups.iter().cloned().collect();
        if groups.is_subset(&self.allowed) {
            self.native.restrict(&list)?;
        } else {
            let health = self
                .native
                .connect(&list, &self.directory.join("feishu/media"))?;
            let capabilities = health["capabilities"]
                .as_array()
                .context("微信原生组件未提供功能列表")?;
            ensure!(
                ["text", "mentions", "image", "gif", "file", "video_download"]
                    .iter()
                    .all(|cap| capabilities.iter().any(|value| value == cap)),
                "本机微信组件功能不完整"
            );
        }
        if let Some(actions) = self.actions.as_mut() {
            if let Err(error) = actions.select(groups) {
                self.actions_error = error.to_string();
            } else {
                self.actions_error.clear();
            }
        }
        if let Some(quotes) = self.quotes.as_mut() {
            quotes.restrict(groups)?;
        }
        self.allowed = groups.clone();
        Ok(())
    }
    fn is_quote(&mut self, group: &str, row: &Value) -> bool {
        self.identity(group, row)
            .is_ok_and(|(content, _)| crate::quotes::reference(&content).is_some())
    }
    fn convert(&mut self, group: &str, row: &Value) -> Result<Option<Value>> {
        if system_message(row) {
            return Ok(None);
        }
        let (content, actor) = self.identity(group, row)?;
        let mut name = if actor.is_empty() {
            String::new()
        } else {
            self.db.display_name(group, &actor)?
        };
        if name.is_empty() || name == actor {
            name = if row["sender_id"] == 2 {
                "我"
            } else {
                "群成员"
            }
            .to_owned();
        }
        let mut message_row = row.clone();
        message_row["content"] = json!(content);
        let kind = text(row, "type");
        let mut media = vec![];
        let mut body = String::new();
        let media_kind = match kind {
            "文本" => {
                body = content.clone();
                None
            }
            "图片" => Some("image"),
            "语音" => Some("voice"),
            "视频" => Some("video"),
            "动画表情" => Some("gif"),
            _ => None,
        };
        if let Some(kind) = media_kind {
            media.push(self.media.inbound(
                &mut self.db,
                Some(&mut self.native),
                group,
                &message_row,
                kind,
            )?);
        } else if kind != "文本" {
            let app_type = xml_text(&content, "type");
            if app_type == "6" || app_type == "74" {
                media.push(self.media.inbound(
                    &mut self.db,
                    Some(&mut self.native),
                    group,
                    &message_row,
                    "file",
                )?);
            } else {
                let mut pieces = vec![];
                for tag in ["title", "des", "url"] {
                    let value = xml_text(&content, tag);
                    if !value.is_empty() && !pieces.contains(&value) {
                        pieces.push(value);
                    }
                }
                body = if pieces.is_empty() {
                    format!(
                        "[{}]",
                        if kind.is_empty() {
                            "暂不支持的消息"
                        } else {
                            kind
                        }
                    )
                } else {
                    pieces.join("\n")
                };
            }
        }
        let mut mentions = vec![];
        let at = source_atlist(row);
        let mut seen = BTreeSet::new();
        for id in at.split(',').map(str::trim).filter(|id| !id.is_empty()) {
            if !seen.insert(id) {
                continue;
            }
            let name = if id == "notify@all" {
                "所有人".to_owned()
            } else {
                self.db.display_name(group, id)?
            };
            mentions.push(json!({"id":id,"name":if name.is_empty(){"群成员"}else{&name}}));
        }
        let timestamp = timestamp(&row["create_time"])?;
        let source_id = source(group, row)?;
        let mut message = json!({"id":Uuid::new_v5(&Uuid::NAMESPACE_URL,source_id.as_bytes()).to_string(),"group":group,"sender":name,"sender_id":actor,"avatar":self.avatar(&actor),"text":body,"media":media,"mentions":mentions,"timestamp":timestamp,"status":"received","source_id":source_id,"outgoing":if actor.is_empty(){row["sender_id"]==2}else{actor==self.db.wxid}});
        if let Some(reference) = crate::quotes::reference(&content) {
            message["text"] = json!(xml_text(&content, "title"));
            let source_row = self.referenced_original(group, &reference)?;
            let source_id = source_row
                .as_ref()
                .map(|row| source(group, row))
                .transpose()?;
            // A referenced image can still be downloading; its reply and stable source ID are ready.
            let original = if let Some(row) = &source_row {
                match self.convert(group, row) {
                    Err(error) if error.is::<crate::media::MediaPending>() => None,
                    result => result?,
                }
            } else {
                None
            };
            let caption = if !text(&reference, "sender").is_empty() {
                text(&reference, "sender").to_owned()
            } else if let Some(original) = &original {
                text(original, "sender").to_owned()
            } else {
                self.db.display_name(group, text(&reference, "chatusr"))?
            };
            let id = source_id
                .as_ref()
                .map(|source| Uuid::new_v5(&Uuid::NAMESPACE_URL, source.as_bytes()).to_string())
                .unwrap_or_else(|| format!("unavailable-{}", text(&reference, "server_id")));
            let preview = if reference["type"] == 3 {
                "[图片]"
            } else {
                text(&reference, "text")
            };
            message["quote"] = json!({"id":id,"sender":if caption.is_empty(){"群成员"}else{&caption},"text":preview,"unavailable":false});
            // Internal proof for repairing legacy unresolved IDs; it is not part of the public Quote model.
            message["_quote_server_id"] = reference["server_id"].clone();
            if let Some(source_id) = source_id {
                message["_quote_source"] = json!(source_id);
            }
            if let Some(original) = original {
                message["_quote_original"] = original;
            }
        }
        Ok(Some(message))
    }
    fn prepare_media(
        &mut self,
        data: &Path,
        id: &str,
        index: usize,
        media: &Value,
    ) -> Result<Step> {
        let prepared = self.media.outgoing(data, id, index, media)?;
        Ok(Step {
            expected: json!({"kind":prepared.kind,"md5":prepared.md5,"name":prepared.name,"size":prepared.size,"gif_attachment":prepared.gif_attachment,"video_attachment":prepared.video_attachment}),
            path: Some(prepared.path),
        })
    }
    fn prepare_quote(&mut self, group: &str, key: &str, body: &str, atlist: &str) -> Result<Step> {
        ensure!(self.allowed.contains(group), "该群未获准引用");
        let locator = locator(group, key)?;
        let row = self.db.reload(group, &locator)?;
        let raw = self.db.raw_message(
            group,
            locator["local_id"].as_i64().context("引用来源编号无效")?,
            text(&locator, "shard"),
        )?;
        ensure!(row["sort_seq"] == raw["sort_seq"], "引用原文定位已改变");
        let source = crate::quotes::prepare_source(group, &row, &raw, &self.db.wxid)?;
        let quotes = self
            .quotes
            .as_mut()
            .with_context(|| format!("微信原生引用不可用：{}", self.quotes_error))?;
        quotes.connect(&self.allowed)?;
        Ok(Step {
            expected: json!({"kind":"quote","text":body,"atlist":atlist,"source":source,"source_id":key}),
            path: None,
        })
    }
    fn submit(&mut self, group: &str, step: &Step, key: &str) -> Result<()> {
        if text(&step.expected, "kind") == "quote" {
            (|| -> Result<()> {
                ensure!(self.allowed.contains(group), "该群已取消转发，引用未提交");
                let current = self.prepare_quote(
                    group,
                    text(&step.expected, "source_id"),
                    text(&step.expected, "text"),
                    text(&step.expected, "atlist"),
                )?;
                ensure!(
                    current.expected["source"] == step.expected["source"],
                    "引用原消息已改变，未提交"
                );
                Ok(())
            })()
            .map_err(|error| {
                anyhow::Error::new(Rejected).context(format!("引用未提交：{error}"))
            })?;
            return self.quotes.as_mut().context("微信引用未连接")?.submit(
                group,
                &step.expected,
                key,
            );
        }
        submission_preflight(self.allowed.contains(group), self.native.pid, || {
            self.native.request("/health", None)
        })?;
        let response = if text(&step.expected, "kind") == "text" {
            self.native.send(
                group,
                text(&step.expected, "text"),
                key,
                text(&step.expected, "atlist"),
            )
        } else {
            self.native.send_media(
                group,
                step.path.as_deref().context("附件路径缺失")?,
                text(&step.expected, "kind"),
                key,
            )
        };
        let response = response.map_err(|error| {
            if error
                .downcast_ref::<NativeHttpError>()
                .is_some_and(NativeHttpError::definite_rejection)
            {
                anyhow::Error::new(Rejected)
            } else {
                error.context("微信发送请求未收到确认，请查看目标群；未自动重发")
            }
        })?;
        ensure!(
            response["status"] == "queued" && response["ret"] == 0,
            "微信发送结果未确认，未自动重发"
        );
        Ok(())
    }
    fn matches(&mut self, group: &str, row: &Value, expected: &Value) -> Result<bool> {
        let (content, actor) = self.identity(group, row)?;
        if row["sender_id"] != 2 || actor != self.db.wxid {
            return Ok(false);
        }
        let kind = text(expected, "kind");
        let actual = text(row, "type");
        if kind == "quote" {
            if !crate::quotes::matches(&content, expected) {
                return Ok(false);
            }
            let at = text(expected, "atlist");
            if at.is_empty() {
                return Ok(true);
            }
            let raw = self.db.raw_message(
                group,
                row["local_id"].as_i64().context("消息编号无效")?,
                text(row, "shard"),
            )?;
            let actual = source_atlist(&raw);
            return Ok(at
                .split(',')
                .all(|id| actual.split(',').any(|other| id == other)));
        }
        if kind == "text" {
            if actual != "文本" || content != text(expected, "text") {
                return Ok(false);
            }
            let at = text(expected, "atlist");
            if !at.is_empty() {
                let mut actual = source_atlist(row);
                if actual.is_empty() {
                    let raw = self.db.raw_message(
                        group,
                        row["local_id"].as_i64().context("微信消息编号缺失")?,
                        text(row, "shard"),
                    )?;
                    actual = source_atlist(&raw);
                }
                if !at
                    .split(',')
                    .all(|id| actual.split(',').any(|candidate| candidate == id))
                {
                    return Ok(false);
                }
            }
            return Ok(true);
        }
        if (kind == "image" && actual != "图片")
            || (kind == "gif" && actual != "动画表情")
            || (kind == "file"
                && !actual.starts_with("文件")
                && !(expected["video_attachment"] == true && actual == "视频"))
        {
            return Ok(false);
        }
        let Ok(doc) = xml(&content) else {
            return Ok(false);
        };
        let size = expected["size"]
            .as_u64()
            .context("附件大小缺失")?
            .to_string();
        if kind == "image" || kind == "gif" {
            let tag = if kind == "gif" { "emoji" } else { "img" };
            return Ok(doc
                .descendants()
                .find(|n| n.has_tag_name(tag))
                .is_some_and(|node| {
                    node.attribute("md5") == Some(text(expected, "md5"))
                        && (kind != "gif" || node.attribute("len") == Some(&size))
                }));
        }
        if expected["video_attachment"] == true && actual == "视频" {
            return Ok(doc
                .descendants()
                .find(|n| n.has_tag_name("videomsg"))
                .is_some_and(|node| {
                    node.attribute("md5") == Some(text(expected, "md5"))
                        && node.attribute("length") == Some(&size)
                }));
        }
        if expected["gif_attachment"] == true && xml_text(&content, "type") == "8" {
            return Ok(xml_text(&content, "fileext").eq_ignore_ascii_case("gif")
                && xml_text(&content, "emoticonmd5") == text(expected, "md5")
                && xml_text(&content, "totallen") == size);
        }
        Ok(matches!(xml_text(&content, "type").as_str(), "6" | "74")
            && xml_text(&content, "title") == text(expected, "name")
            && xml_text(&content, "md5") == text(expected, "md5")
            && xml_text(&content, "totallen") == size)
    }
    fn server_id(&mut self, group: &str, row: &Value) -> Result<String> {
        let raw = self.db.raw_message(
            group,
            row["local_id"].as_i64().context("微信消息编号缺失")?,
            text(row, "shard"),
        )?;
        ensure!(raw["sort_seq"] == row["sort_seq"], "微信确认消息定位已变化");
        let value = &raw["server_id"];
        Ok(if let Some(id) = value.as_i64() {
            if id == 0 {
                String::new()
            } else {
                (id as u64).to_string()
            }
        } else if let Some(id) = value.as_u64() {
            if id == 0 {
                String::new()
            } else {
                id.to_string()
            }
        } else {
            String::new()
        })
    }
    fn recall_supported(&self) -> bool {
        false
    }
    fn recall_info(&mut self, _group: &str, _key: &str, _now: f64) -> Result<Value> {
        anyhow::bail!(crate::actions::BACKGROUND_RECALL_UNAVAILABLE)
    }
    fn recall_submit(&mut self, _group: &str, _info: &Value, _id: &str) -> Result<()> {
        Err(anyhow::Error::new(crate::recall::DefinitelyNotSubmitted)
            .context(crate::actions::BACKGROUND_RECALL_UNAVAILABLE))
    }
    fn recall_progress(&mut self, group: &str, info: &Value, _id: &str) -> Result<Option<bool>> {
        let row = self.db.reload(group, &info["locator"])?;
        let server = self.server_id(group, &row)?;
        let body = text(&row, "content");
        let revoked = system_message(&row)
            && !text(info, "server_id").is_empty()
            && server == text(info, "server_id")
            && xml(body).is_ok_and(|doc| {
                doc.root_element().has_tag_name("sysmsg")
                    && doc.root_element().attribute("type") == Some("revokemsg")
                    && doc.descendants().any(|node| node.has_tag_name("revokemsg"))
            });
        Ok(revoked.then_some(true))
    }
    fn release(&mut self) {
        self.db.release();
    }
}

fn message_identity(
    group: &str,
    row: &Value,
    self_id: &str,
    mut display: impl FnMut(&str) -> Result<String>,
) -> Result<(String, String)> {
    let content = text(row, "content");
    let mut actor = text(row, "sender_username").to_owned();
    if actor.is_empty() && row["sender_id"] == 2 {
        actor = self_id.to_owned();
    }
    if group.ends_with("@chatroom") {
        // A sender envelope is a complete first line, never a later line inside the message body.
        if let Some((line, body)) = content.split_once('\n') {
            if let Some(prefix) = line.strip_suffix('\r').unwrap_or(line).strip_suffix(':') {
                if !prefix.is_empty()
                    && prefix.len() <= 128
                    && prefix
                        .bytes()
                        .all(|c| c.is_ascii_alphanumeric() || b"_-".contains(&c))
                    && (prefix == actor
                        || (actor.is_empty()
                            && (prefix.starts_with("wxid_") || display(prefix)? != prefix)))
                {
                    return Ok((body.to_owned(), prefix.to_owned()));
                }
            }
        }
    }
    Ok((content.to_owned(), actor))
}

fn source_text(value: &Value) -> Cow<'_, str> {
    const LIMIT: usize = 2 * 1024 * 1024;
    if let Some(value) = value.as_str() {
        return Cow::Borrowed(if value.len() <= LIMIT { value } else { "" });
    }
    if let Some(bytes) = value.as_array().filter(|bytes| bytes.len() <= LIMIT) {
        let bytes: Option<Vec<u8>> = bytes
            .iter()
            .map(|value| value.as_u64().and_then(|value| u8::try_from(value).ok()))
            .collect();
        if let Some(mut bytes) = bytes {
            if bytes.starts_with(b"\x28\xb5\x2f\xfd") {
                // WeChat also stores msgsource as Zstd BLOBs. Keep the same
                // XML/@ checks after decoding, with bounded input/window/output.
                let Ok(mut decoder) = zstd::stream::read::Decoder::new(bytes.as_slice()) else {
                    return Cow::Borrowed("");
                };
                if decoder.window_log_max(21).is_err() {
                    return Cow::Borrowed("");
                }
                let mut decoded = Vec::new();
                if decoder
                    .take(LIMIT as u64 + 1)
                    .read_to_end(&mut decoded)
                    .is_err()
                    || decoded.len() > LIMIT
                {
                    return Cow::Borrowed("");
                }
                bytes = decoded;
            }
            if let Ok(value) = String::from_utf8(bytes) {
                return Cow::Owned(value);
            }
        }
    }
    Cow::Borrowed("")
}

fn source_atlist(row: &Value) -> String {
    xml_text(&source_text(&row["source"]), "atuserlist")
}

fn submission_preflight(
    allowed: bool,
    pid: Option<u32>,
    health: impl FnOnce() -> Result<Value>,
) -> Result<()> {
    let result = (|| -> Result<()> {
        ensure!(allowed, "该群未获准本机发送");
        let health = health()?;
        let has_error = match &health["error"] {
            Value::Null => false,
            Value::Bool(value) => *value,
            Value::Number(value) => value.as_f64() != Some(0.0),
            Value::String(value) => !value.is_empty(),
            Value::Array(value) => !value.is_empty(),
            Value::Object(value) => !value.is_empty(),
        };
        ensure!(
            pid.is_some_and(|pid| health["pid"].as_u64() == Some(u64::from(pid))) && !has_error,
            "微信主进程或组件状态发生变化"
        );
        Ok(())
    })();
    result.map_err(|error| {
        anyhow::Error::new(Rejected).context(format!("微信发送前检查失败，消息未提交：{error}"))
    })
}

fn locator(group: &str, key: &str) -> Result<Value> {
    let rest = key
        .strip_prefix(&format!("wx:{group}:"))
        .context("撤回来源不属于当前群")?;
    let mut parts = rest.rsplitn(3, ':');
    let seq = parts.next().context("撤回游标缺失")?.parse::<i64>()?;
    let id = parts.next().context("撤回编号缺失")?.parse::<u32>()?;
    let shard = parts.next().context("撤回分片缺失")?;
    ensure!(
        id > 0 && !shard.is_empty() && shard.len() <= 256 && !shard.contains(['/', '\\', '\0']),
        "撤回来源定位无效"
    );
    Ok(json!({"sort_seq":seq,"local_id":id,"shard":shard}))
}

pub fn timestamp(value: &Value) -> Result<i64> {
    let stamp = if let Some(value) = value.as_f64() {
        value
    } else if let Some(value) = value.as_str() {
        if let Ok(value) = value.parse::<f64>() {
            value
        } else {
            return Ok(chrono::DateTime::parse_from_rfc3339(value)?.timestamp_millis());
        }
    } else {
        anyhow::bail!("微信消息时间无效");
    };
    ensure!(stamp.is_finite(), "微信消息时间无效");
    Ok(if stamp.abs() < 10_000_000_000.0 {
        (stamp * 1000.0) as i64
    } else {
        stamp as i64
    })
}

#[cfg(test)]
mod tests;
