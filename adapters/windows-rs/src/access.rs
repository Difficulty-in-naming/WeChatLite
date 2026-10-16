use crate::actions::Actions;
use crate::{
    db::Db,
    engine::{Backend, Rejected, Step, source, system_message, text, xml, xml_text},
    media::{Media, avatar_data},
    native::{NativeClient, NativeHttpError},
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    borrow::Cow,
    collections::{BTreeSet, HashMap, VecDeque},
    io::Read,
    path::{Path, PathBuf},
    time::{Duration, Instant},
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
        use rusqlite::OptionalExtension;
        if reference["type"] != 1 {
            return Ok(None);
        }
        let server = text(reference, "server_id")
            .parse::<u64>()
            .context("引用来源编号无效")?;
        let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
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
                .query_row(
                    &format!("SELECT local_id,sort_seq FROM {table} WHERE server_id=?1 LIMIT 1"),
                    [server as i64],
                    |row| Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?)),
                )
                .optional()?;
            if let Some((id, sequence)) = found {
                let locator =
                    json!({"local_id":id,"sort_seq":sequence,"shard":rel.replace('/',"__")});
                let row = self.db.reload(group, &locator)?;
                let raw = self.db.raw_message(group, id, text(&locator, "shard"))?;
                let Ok(source) = crate::quotes::prepare_source(group, &row, &raw, &self.db.wxid)
                else {
                    return Ok(None);
                };
                if source["server_id"] != reference["server_id"]
                    || source["fromusr"] != reference["fromusr"]
                    || source["chatusr"] != reference["chatusr"]
                    || text(&source, "content")
                        .replace("\r\n", "\n")
                        .replace('\r', "\n")
                        != text(reference, "text")
                            .replace("\r\n", "\n")
                            .replace('\r', "\n")
                {
                    return Ok(None);
                }
                // Only validated type-1 source rows reach this recursion; no nested quote expansion.
                return self.convert(group, &row);
            }
        }
        Ok(None)
    }
    fn recall_record(&mut self, group: &str, key: &str, now: f64) -> Result<Value> {
        ensure!(self.allowed.contains(group), "撤回群尚未获准");
        let locator = locator(group, key)?;
        let row = self.db.reload(group, &locator)?;
        let raw = self.db.raw_message(
            group,
            row["local_id"].as_i64().context("撤回消息编号缺失")?,
            text(&row, "shard"),
        )?;
        recall_target(group, &row, &raw, &self.db.wxid, now)
    }
    fn recall_prepare(&mut self, group: &str, key: &str, now: f64) -> Result<Value> {
        let mut info = self.recall_record(group, key, now)?;
        // Only the requested target's fingerprint is persisted, never chat history or UI pointers.
        info.as_object_mut().unwrap().remove("ui_text");
        Ok(info)
    }
    fn recall_snapshot(&mut self, group: &str, key: &str, now: f64) -> Result<Value> {
        let started = Instant::now();
        let history = self.db.recall_history_snapshot()?;
        let mut info = self.recall_record(group, key, now)?;
        let locator = info["locator"].clone();
        let group_name = recall_group_name(&self.db.groups()?, group)?;
        self.db.verify_recall_name(group, &group_name)?;

        let mut before = None;
        let mut scanned = 0;
        let mut context: Vec<RecallText> = Vec::new();
        let mut context_bytes = 0;
        let mut broken = false;
        // Gather a bounded UI context; the separate streaming pass proves uniqueness.
        'history: while scanned < 1000 && context.len() < UI_CONTEXT_MESSAGES {
            let rows = self
                .db
                .older(group, before.as_ref(), 61.min(1000 - scanned))?;
            if rows.is_empty() {
                break;
            }
            scanned += rows.len();
            before = rows.last().cloned();
            for row in rows {
                if text(&row, "type") != "文本" {
                    continue;
                }
                let raw = self.db.raw_message(
                    group,
                    row["local_id"].as_i64().context("撤回上下文编号缺失")?,
                    text(&row, "shard"),
                )?;
                if let Ok(entry) = recall_text(group, &row, &raw, &self.db.wxid) {
                    if context.len() == UI_CONTEXT_MESSAGES
                        || context_bytes + entry.text.len() > UI_CONTEXT_BYTES
                    {
                        break 'history;
                    }
                    if let Some(newer) = context.last_mut() {
                        newer.break_before = broken;
                    }
                    broken = false;
                    context_bytes += entry.text.len();
                    context.push(entry);
                } else {
                    broken = true;
                }
            }
        }
        let context = recall_context(context, &locator, text(&info, "ui_text"))?;
        let unique = self.recall_unique_contexts(
            group,
            &history,
            &context,
            &locator,
            started,
            RecallScanLimits::default(),
        )?;

        // History and raw rows are separate reads; fail closed if the target changed meanwhile.
        let current = self.db.reload(group, &locator)?;
        let raw = self.db.raw_message(
            group,
            current["local_id"].as_i64().context("撤回消息编号缺失")?,
            text(&current, "shard"),
        )?;
        let current = recall_target(group, &current, &raw, &self.db.wxid, now)?;
        unchanged_recall_target(&info, &current)?;
        ensure!(
            recall_group_name(&self.db.groups()?, group)? == group_name,
            "完整历史校验期间群名称发生变化，撤回未提交"
        );
        self.db.verify_recall_history_snapshot(&history)?;
        self.db.verify_recall_name(group, &group_name)?;
        info["group_name"] = json!(group_name);
        info["ui_context"] = json!(context);
        info["ui_unique_contexts"] = json!(unique);
        info["ui_history_gate"] = self.db.ui_history_gate(&history);
        Ok(info)
    }
    fn recall_unique_contexts(
        &mut self,
        group: &str,
        snapshot: &crate::db::RecallHistorySnapshot,
        context: &[Value],
        target: &Value,
        started: Instant,
        limits: RecallScanLimits,
    ) -> Result<Vec<Value>> {
        let mut proof = RecallUniqueness::new(context, target)?;
        let mut position = None;
        let mut scanned = 0;
        loop {
            limits.check(started, scanned)?;
            let rows = self
                .db
                .recall_history_page(group, snapshot, position.as_ref())?;
            limits.check(started, scanned)?;
            if rows.is_empty() {
                self.db.verify_recall_history_snapshot(snapshot)?;
                return proof.finish(true);
            }
            for row in &rows {
                scanned += 1;
                limits.check(started, scanned)?;
                proof.observe(group, row, &self.db.wxid)?;
            }
            position = rows.last().cloned();
        }
    }
    fn recall_refresh(&mut self, group: &str, info: &Value, now: f64) -> Result<Value> {
        let key = source(group, &info["locator"])?;
        let current = self.recall_snapshot(group, &key, now)?;
        unchanged_recall_target(info, &current)?;
        Ok(current)
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
            let original = self.referenced_original(group, &reference)?;
            let caption = if !text(&reference, "sender").is_empty() {
                text(&reference, "sender").to_owned()
            } else if let Some(original) = &original {
                text(original, "sender").to_owned()
            } else {
                self.db.display_name(group, text(&reference, "chatusr"))?
            };
            let id = original
                .as_ref()
                .map(|value| text(value, "id").to_owned())
                .unwrap_or_else(|| format!("unavailable-{}", text(&reference, "server_id")));
            message["quote"] = json!({"id":id,"sender":if caption.is_empty(){"群成员"}else{&caption},"text":reference["text"],"unavailable":false});
            if let Some(original) = original {
                message["_quote_source"] = original["source_id"].clone();
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
    fn recall_info(&mut self, group: &str, key: &str, now: f64) -> Result<Value> {
        ensure!(
            self.actions.is_some() && self.actions_error.is_empty(),
            "微信菜单撤回尚不可用：{}",
            self.actions_error
        );
        self.recall_prepare(group, key, now)
    }
    fn recall_submit(&mut self, group: &str, info: &Value, id: &str) -> Result<()> {
        let fresh = (|| {
            ensure!(self.actions_error.is_empty(), "{}", self.actions_error);
            let mut fresh = self.recall_refresh(group, info, crate::now())?;
            let (pid, base) = crate::native::verified_ui_process()?;
            fresh["ui_account_gate"] = crate::db::verify_ui_account(&self.db.wxid, pid, base)?;
            recall_time(&fresh["create_time"], crate::now())?;
            Ok(fresh)
        })()
        .map_err(|error: anyhow::Error| {
            anyhow::Error::new(crate::recall::DefinitelyNotSubmitted)
                .context(format!("微信撤回前检查失败，未提交：{error}"))
        })?;
        self.actions
            .as_mut()
            .ok_or_else(|| {
                anyhow::Error::new(crate::recall::DefinitelyNotSubmitted)
                    .context("微信撤回组件未连接")
            })?
            .submit(group, &fresh, id)
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

const UI_CONTEXT_MESSAGES: usize = 200;
const UI_CONTEXT_BYTES: usize = 1024 * 1024;

struct RecallText {
    cursor: (i64, String, i64),
    text: String,
    outgoing: bool,
    break_before: bool,
}

fn recall_text(group: &str, row: &Value, raw: &Value, wxid: &str) -> Result<RecallText> {
    // This validator only decodes/cross-checks DB fields; it never accesses the quote component.
    crate::quotes::prepare_source(group, row, raw, wxid)
        .context("撤回定位仅支持已确认、可完整读取的原微信纯文本")?;
    let (body, actor) = message_identity(group, row, wxid, |id| Ok(id.to_owned()))?;
    let outgoing = row["sender_id"] == 2;
    ensure!(!outgoing || actor == wxid, "撤回消息归属发生变化");
    let locator = locator(group, &source(group, row)?)?;
    Ok(RecallText {
        cursor: recall_cursor(&locator)?,
        text: body,
        outgoing,
        break_before: false,
    })
}

fn recall_cursor(locator: &Value) -> Result<(i64, String, i64)> {
    Ok((
        locator["sort_seq"].as_i64().context("撤回游标无效")?,
        locator["shard"].as_str().context("撤回分片无效")?.into(),
        locator["local_id"].as_i64().context("撤回编号无效")?,
    ))
}

fn recall_target(group: &str, row: &Value, raw: &Value, wxid: &str, now: f64) -> Result<Value> {
    let entry = recall_text(group, row, raw, wxid)?;
    ensure!(entry.outgoing, "只能撤回当前账号自己的消息");
    recall_time(&row["create_time"], now)?;
    let prepared = crate::quotes::prepare_source(group, row, raw, wxid)?;
    let locator = locator(group, &source(group, row)?)?;
    Ok(
        json!({"group":group,"server_id":prepared["server_id"],"create_time":row["create_time"],
        "local_id":row["local_id"],"locator":locator,
        "ui_text_hash":hex::encode(Sha256::digest(entry.text.as_bytes())),"ui_text":entry.text}),
    )
}

fn recall_time(stamp: &Value, now: f64) -> Result<()> {
    let stamp = stamp.as_i64().context("原微信消息时间无效")?;
    ensure!(
        stamp > 0 && now.is_finite() && now >= stamp as f64 && now - (stamp as f64) < 180.0,
        "已超过3分钟撤回时限"
    );
    Ok(())
}

fn recall_group_name(groups: &[Value], group: &str) -> Result<String> {
    let mut matches = groups.iter().filter(|row| text(row, "username") == group);
    let target = matches.next().context("当前群已不在本机微信群列表中")?;
    ensure!(matches.next().is_none(), "本机微信群编号不唯一");
    let name = text(target, "name");
    ensure!(
        !name.trim().is_empty() && !name.contains('\0'),
        "当前群名称无效"
    );
    ensure!(
        groups
            .iter()
            .filter(|row| text(row, "name") == name)
            .count()
            == 1,
        "本机微信存在同名群，无法安全定位撤回菜单"
    );
    Ok(name.to_owned())
}

fn recall_context(
    mut entries: Vec<RecallText>,
    target: &Value,
    target_text: &str,
) -> Result<Vec<Value>> {
    entries.sort_by(|left, right| left.cursor.cmp(&right.cursor));
    let mut bytes = 0;
    let count = entries
        .iter()
        .rev()
        .take(UI_CONTEXT_MESSAGES)
        .take_while(|entry| {
            bytes += entry.text.len();
            bytes <= UI_CONTEXT_BYTES
        })
        .count();
    entries.drain(..entries.len() - count);
    let target = recall_cursor(target)?;
    ensure!(
        entries
            .iter()
            .filter(|entry| entry.cursor == target)
            .count()
            == 1,
        "目标不在可唯一验证的最近文本记录中，撤回未提交"
    );
    entries
        .into_iter()
        .map(|entry| {
            let target = entry.cursor == target;
            ensure!(
                !target || (entry.outgoing && entry.text == target_text),
                "撤回上下文中的目标内容或归属已变化"
            );
            let mut value = json!({"text":entry.text,"outgoing":entry.outgoing,"target":target});
            if entry.break_before {
                value["break_before"] = true.into();
            }
            Ok(value)
        })
        .collect()
}

#[derive(Clone, Copy)]
struct RecallScanLimits {
    rows: usize,
    duration: Duration,
}
impl Default for RecallScanLimits {
    fn default() -> Self {
        Self {
            rows: 200_000,
            duration: Duration::from_secs(10),
        }
    }
}
impl RecallScanLimits {
    fn check(self, started: Instant, rows: usize) -> Result<()> {
        ensure!(
            rows <= self.rows && started.elapsed() < self.duration,
            "完整历史唯一性校验超时或超过20万条容量，撤回未提交"
        );
        Ok(())
    }
}

struct RecallCandidate {
    start: usize,
    texts: Vec<String>,
    target_offset: usize,
    matches: u8,
    includes_target: bool,
}
struct RecallUniqueness {
    candidates: Vec<RecallCandidate>,
    target: (i64, String, i64),
    previous: Option<(i64, String, i64)>,
    window: VecDeque<((i64, String, i64), String)>,
}
impl RecallUniqueness {
    fn new(context: &[Value], target: &Value) -> Result<Self> {
        let targets: Vec<_> = context
            .iter()
            .enumerate()
            .filter(|(_, entry)| entry["target"] == true)
            .map(|(index, _)| index)
            .collect();
        ensure!(
            targets.len() == 1 && !context.is_empty(),
            "缺少可验证的目标文本，撤回未提交"
        );
        let target_index = targets[0];
        let mut candidates = Vec::new();
        for length in 1..=3.min(context.len()) {
            for start in
                target_index.saturating_sub(length - 1)..=target_index.min(context.len() - length)
            {
                if context[start + 1..start + length]
                    .iter()
                    .any(|entry| entry["break_before"] == true)
                {
                    continue;
                }
                let texts: Vec<_> = (0..length)
                    .map(|offset| text(&context[start + offset], "text").to_owned())
                    .collect();
                ensure!(
                    texts.iter().all(|text| !text.is_empty()),
                    "连续文本原文无效，撤回未提交"
                );
                candidates.push(RecallCandidate {
                    start,
                    texts,
                    target_offset: target_index - start,
                    matches: 0,
                    includes_target: false,
                });
            }
        }
        ensure!(
            !candidates.is_empty(),
            "目标附近文本存在不可验证的断点，撤回未提交"
        );
        Ok(Self {
            candidates,
            target: recall_cursor(target)?,
            previous: None,
            window: VecDeque::new(),
        })
    }
    fn observe(&mut self, group: &str, row: &Value, wxid: &str) -> Result<()> {
        let cursor = recall_cursor(row)?;
        ensure!(
            self.previous
                .as_ref()
                .is_none_or(|previous| previous < &cursor),
            "完整历史分页未严格前进，撤回未提交"
        );
        self.previous = Some(cursor.clone());
        if row["is_text"] != true {
            return Ok(());
        }
        // Unknown text cannot establish global absence of a duplicate; reject the entire proof.
        ensure!(
            row["local_type"] == 1
                && row["content"].is_string()
                && !text(row, "sender_username").is_empty(),
            "群历史存在无法完整验证的纯文本，撤回未提交"
        );
        let (body, _) = message_identity(group, row, wxid, |id| Ok(id.to_owned()))?;
        ensure!(!body.is_empty(), "群历史存在空白原文，撤回未提交");
        self.window.push_back((cursor, body));
        if self.window.len() > 3 {
            self.window.pop_front();
        }
        for candidate in &mut self.candidates {
            if self.window.len() < candidate.texts.len() {
                continue;
            }
            let offset = self.window.len() - candidate.texts.len();
            if self
                .window
                .iter()
                .skip(offset)
                .zip(&candidate.texts)
                .all(|((_, body), expected)| body == expected)
            {
                candidate.matches = candidate.matches.saturating_add(1).min(2);
                candidate.includes_target |=
                    self.window[offset + candidate.target_offset].0 == self.target;
            }
        }
        Ok(())
    }
    fn finish(self, complete: bool) -> Result<Vec<Value>> {
        ensure!(complete, "历史扫描未完成，不能证明唯一位置，撤回未提交");
        let shortest = self
            .candidates
            .iter()
            .filter(|candidate| candidate.matches == 1 && candidate.includes_target)
            .map(|candidate| candidate.texts.len())
            .min();
        let unique: Vec<_> = self
            .candidates
            .into_iter()
            .filter(|candidate| {
                candidate.matches == 1
                    && candidate.includes_target
                    && Some(candidate.texts.len()) == shortest
            })
            .map(|candidate| json!({"start":candidate.start,"length":candidate.texts.len()}))
            .collect();
        ensure!(
            !unique.is_empty(),
            "目标原文及附近文本在全群历史中不唯一，撤回未提交"
        );
        Ok(unique)
    }
}

fn unchanged_recall_target(previous: &Value, current: &Value) -> Result<()> {
    for field in [
        "group",
        "server_id",
        "create_time",
        "local_id",
        "locator",
        "ui_text_hash",
    ] {
        ensure!(
            !previous[field].is_null() && previous[field] == current[field],
            "撤回原消息的 {field} 已变化，未使用旧记录执行"
        );
    }
    Ok(())
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
