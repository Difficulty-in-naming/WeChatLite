//! Native type-57 text quotes. Source fields always come from fresh same-group DB rows.
use crate::{
    engine::{Rejected, text},
    native::{NativeClient, NativeHttpError, request_id},
};
use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use std::{
    borrow::Cow,
    collections::BTreeSet,
    io::Read,
    path::PathBuf,
    time::{SystemTime, UNIX_EPOCH},
};

const MAX_TEXT: usize = 24_000;
const MAX_DB_TEXT: usize = MAX_TEXT + 131; // 128-byte actor, colon and CRLF.

pub struct Quotes {
    client: NativeClient,
    directory: PathBuf,
    connected: bool,
    allowed: BTreeSet<String>,
}

impl Quotes {
    pub fn new(directory: PathBuf, wxid: &str) -> Result<Self> {
        // Revoke stale quote grants even if the optional binary is absent. Actions uses a different root.
        let mut previous = NativeClient::new(directory.join("native-quotes"));
        previous.restrict(&[])?;
        drop(previous);
        ensure!(member_id(wxid), "当前微信账号编号无效");
        let hash = include_str!("../vendor/WeChatQuote.sha256").trim();
        ensure!(
            hash.len() == 64
                && hash
                    .bytes()
                    .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)),
            "原生引用组件尚未构建或指纹无效"
        );
        let executable = std::env::current_exe()?;
        let binary = std::fs::read(
            executable
                .parent()
                .context("程序目录缺失")?
                .join("WeChatQuote.dll"),
        )
        .context("缺少独立原生引用组件 WeChatQuote.dll")?;
        let mut client = NativeClient::with_binary(
            directory.join("native-quotes"),
            binary,
            hash,
            json!({"self_wxid":wxid,"capability":"quote_text"}),
        );
        client.restrict(&[])?;
        Ok(Self {
            client,
            directory,
            connected: false,
            allowed: BTreeSet::new(),
        })
    }

    /// Only tighten existing permissions; this never loads a DLL or grants another group.
    pub fn restrict(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        let narrowed: BTreeSet<_> = self.allowed.intersection(groups).cloned().collect();
        self.client
            .restrict(&narrowed.iter().cloned().collect::<Vec<_>>())?;
        self.allowed = narrowed;
        if self.allowed.is_empty() {
            if self.connected {
                let _ = self.client.request("/stop", Some(&json!({})));
            }
            self.connected = false;
        }
        Ok(())
    }

    /// Explicit authorization loads only the separate hook-free Quote DLL and runs its ABI probe.
    pub fn connect(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        if groups.is_empty() {
            return self.restrict(groups);
        }
        ensure!(
            groups.iter().all(|group| group_id(group)),
            "原生引用群编号无效"
        );
        let result = (|| -> Result<()> {
            let health = self.client.connect(
                &groups.iter().cloned().collect::<Vec<_>>(),
                &self.directory.join("feishu/media"),
            )?;
            ensure!(
                quote_health(&health, self.client.pid, false),
                "原生引用组件身份或构造探针未通过"
            );
            let started = self.client.request("/start", Some(&json!({})))?;
            ensure!(started["enabled"] == true, "原生引用组件未启用");
            Ok(())
        })();
        if let Err(error) = result {
            self.connected = false;
            self.allowed.clear();
            self.client
                .restrict(&[])
                .context("原生引用启动失败后的授权清理未确认")?;
            return Err(error);
        }
        self.allowed = groups.clone();
        self.connected = true;
        Ok(())
    }

    pub fn submit(&mut self, group: &str, expected: &Value, key: &str) -> Result<()> {
        let preflight = (|| -> Result<()> {
            ensure!(
                self.connected && self.allowed.contains(group),
                "该群尚未获准原生引用"
            );
            validate_expected(group, expected)?;
            ensure!(!key.is_empty() && key.len() <= 4096, "原生引用任务编号无效");
            let health = self.client.request("/health", None)?;
            ensure!(
                quote_health(&health, self.client.pid, true),
                "原生引用组件状态变化"
            );
            Ok(())
        })();
        preflight.map_err(|error| {
            anyhow::Error::new(Rejected).context(format!("引用未提交微信：{error}"))
        })?;
        let id = request_id(key);
        let response = self
            .client
            .request(
                "/quote",
                Some(&json!({
                    "group":group,"source":expected["source"],"text":expected["text"],
                    "atlist":text(expected,"atlist"),"request_id":id,
                })),
            )
            .map_err(|error| {
                if error
                    .downcast_ref::<NativeHttpError>()
                    .is_some_and(NativeHttpError::definite_rejection)
                {
                    anyhow::Error::new(Rejected).context(format!("原生引用未提交：{error}"))
                } else {
                    error.context("原生引用提交结果未确认，未自动重发")
                }
            })?;
        ensure!(
            valid_receipt(&response, group, &id)
                && response["status"] == "queued"
                && response["source_server_id"] == expected["source"]["server_id"],
            "原生引用回执未确认，未自动重发"
        );
        Ok(())
    }

    /// A queue receipt is a locator, never delivery confirmation. DB XML/server_id remains authoritative.
    pub fn progress(&mut self, group: &str, key: &str) -> Result<Option<Value>> {
        if !self.allowed.contains(group) {
            return Ok(None);
        }
        ensure!(
            group_id(group) && !key.is_empty() && key.len() <= 4096,
            "原生引用回执定位无效"
        );
        if !self.connected && !self.client.attach_existing()? {
            return Ok(None);
        }
        let id = request_id(key);
        match self
            .client
            .request(&format!("/result?group={group}&request_id={id}"), None)
        {
            Ok(value) => {
                ensure!(
                    valid_receipt(&value, group, &id),
                    "原生引用回执不属于该任务"
                );
                Ok(Some(value))
            }
            Err(error)
                if error
                    .downcast_ref::<NativeHttpError>()
                    .is_some_and(|error| error.code == 404) =>
            {
                Ok(None)
            }
            Err(error) => Err(error),
        }
    }
}

impl Drop for Quotes {
    fn drop(&mut self) {
        let _ = self.restrict(&BTreeSet::new());
    }
}

fn group_id(group: &str) -> bool {
    group.strip_suffix("@chatroom").is_some_and(|number| {
        (5..=24).contains(&number.len()) && number.bytes().all(|c| c.is_ascii_digit())
    })
}
fn member_id(member: &str) -> bool {
    !member.is_empty()
        && member.len() <= 128
        && member
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_-".contains(&c))
}
fn bounded_text(value: &str) -> bool {
    !value.is_empty() && value.len() <= MAX_TEXT && !value.contains('\0')
}
fn server_id(value: &Value) -> Result<String> {
    let number = if let Some(number) = value.as_i64() {
        number as u64 // SQLite stores unsigned message IDs using their signed 64-bit representation.
    } else if let Some(number) = value.as_u64() {
        number
    } else if let Some(number) = value
        .as_str()
        .filter(|v| !v.is_empty() && v.len() <= 20 && v.bytes().all(|c| c.is_ascii_digit()))
    {
        number.parse::<u64>()?
    } else {
        bail!("引用原消息服务端编号无效")
    };
    ensure!(number != 0, "引用原消息尚未获微信服务端确认");
    Ok(number.to_string())
}
fn positive_u32(value: &Value, label: &str) -> Result<u32> {
    let number = value
        .as_u64()
        .with_context(|| format!("引用原消息{label}无效"))?;
    ensure!(
        (1..=u32::MAX as u64).contains(&number),
        "引用原消息{label}无效"
    );
    Ok(number as u32)
}
fn db_text(value: &Value) -> Result<String> {
    let bytes = if let Some(value) = value.as_str() {
        ensure!(value.len() <= MAX_DB_TEXT, "引用原文超过微信原生容量");
        return Ok(value.to_owned());
    } else {
        let values = value.as_array().context("引用原文编码尚不可验证")?;
        ensure!(values.len() <= 2 * 1024 * 1024, "引用原文字段过大");
        values
            .iter()
            .map(|value| {
                value
                    .as_u64()
                    .and_then(|v| u8::try_from(v).ok())
                    .context("引用原文字节无效")
            })
            .collect::<Result<Vec<_>>>()?
    };
    let bytes = if bytes.starts_with(b"\x28\xb5\x2f\xfd") {
        let mut decoder = zstd::stream::read::Decoder::new(bytes.as_slice())?;
        decoder.window_log_max(23)?;
        let mut decoded = Vec::new();
        decoder
            .take((MAX_DB_TEXT + 1) as u64)
            .read_to_end(&mut decoded)?;
        ensure!(decoded.len() <= MAX_DB_TEXT, "引用原文超过微信原生容量");
        decoded
    } else {
        bytes
    };
    ensure!(bytes.len() <= MAX_DB_TEXT, "引用原文超过微信原生容量");
    String::from_utf8(bytes).context("引用原文并非完整UTF-8，未使用显示占位文字替代")
}
fn remove_envelope<'a>(content: &'a str, actor: &str) -> &'a str {
    if let Some((line, body)) = content.split_once('\n') {
        if line.strip_suffix('\r').unwrap_or(line).strip_suffix(':') == Some(actor) {
            return body;
        }
    }
    content
}

/// `row` and `raw` must be freshly read from the same group's DB table using one exact locator.
/// Their shard/local_id/sort_seq/time/actor and raw server ID are cross-checked before constructing wire fields.
pub fn prepare_source(group: &str, row: &Value, raw: &Value, wxid: &str) -> Result<Value> {
    ensure!(
        text(row, "type") == "文本" && raw["local_type"].as_i64() == Some(1),
        "原生引用目前仅支持原微信文本消息，不会改发摘要"
    );
    source_fields(group, row, raw, wxid)
}

/// Resolve received references without enabling additional native send types.
pub fn matches_reference(
    group: &str,
    row: &Value,
    raw: &Value,
    wxid: &str,
    reference: &Value,
) -> bool {
    let label = match reference["type"].as_u64() {
        Some(1) => "文本",
        Some(3) => "图片",
        _ => return false,
    };
    if row["type"] != label || raw["local_type"] != reference["type"] {
        return false;
    }
    let Ok(source) = source_fields(group, row, raw, wxid) else {
        return false;
    };
    source["server_id"] == reference["server_id"]
        // Received cards use either the original author or the conversation as fromusr.
        // Neither spelling may override the actor confirmed by this exact group's DB row.
        && (reference["fromusr"] == source["chatusr"] || reference["fromusr"] == group)
        && source["chatusr"] == reference["chatusr"]
        && if reference["type"] == 3 {
            // CDN URLs, thumbnail fields and XML formatting can change when WeChat quotes an image.
            // The exact group/server ID/DB actor above plus the original image digest identify it.
            image_digest(text(&source, "content")).is_some_and(|digest| {
                image_digest(remove_envelope(text(reference, "text"), text(&source, "chatusr"))).as_deref() == Some(digest.as_str())
            })
        } else {
            xml_newlines(text(&source, "content")) == xml_newlines(text(reference, "text"))
        }
}

fn image_digest(content: &str) -> Option<String> {
    if content.len() > 2 * 1024 * 1024
        || content.contains("<!DOCTYPE")
        || content.contains("<!ENTITY")
    {
        return None;
    }
    let doc = roxmltree::Document::parse(content).ok()?;
    let root = doc.root_element();
    if !root.has_tag_name("msg") {
        return None;
    }
    let mut images = root.descendants().filter(|node| node.has_tag_name("img"));
    let image = images.next()?;
    if image.parent() != Some(root) || images.next().is_some() {
        return None;
    }
    let digest = image.attribute("md5")?;
    (digest.len() == 32 && digest.bytes().all(|byte| byte.is_ascii_hexdigit()))
        .then(|| digest.to_ascii_lowercase())
}

#[cfg(test)]
pub fn image_reference_shape(reference: &Value) -> Value {
    let body = text(reference, "text");
    let doc = roxmltree::Document::parse(body).ok();
    let image = doc
        .as_ref()
        .and_then(|doc| doc.descendants().find(|node| node.has_tag_name("img")));
    let digest = image.and_then(|image| image.attribute("md5"));
    let envelope_body = remove_envelope(body, text(reference, "chatusr"));
    let unescaped = if !body.contains('<') && body.contains('&') {
        let wrapped = format!("<probe>{body}</probe>");
        roxmltree::Document::parse(&wrapped)
            .ok()
            .filter(|doc| !doc.root_element().children().any(|node| node.is_element()))
            .map(|doc| {
                doc.root_element()
                    .children()
                    .filter(|node| node.is_text())
                    .filter_map(|node| node.text())
                    .collect::<String>()
            })
    } else {
        None
    };
    json!({"reference_body_empty":body.trim().is_empty(),"reference_body_is_image_label":body.trim()=="[图片]",
        "reference_body_xml":doc.is_some(),"reference_body_root_msg":doc.as_ref().is_some_and(|doc|doc.root_element().has_tag_name("msg")),
        "reference_body_root_img":doc.as_ref().is_some_and(|doc|doc.root_element().has_tag_name("img")),
        "reference_body_has_img":image.is_some(),"reference_body_md5_attr":digest.is_some(),
        "reference_body_md5_valid":digest.is_some_and(|digest|digest.len()==32 && digest.bytes().all(|byte|byte.is_ascii_hexdigit())),
        "reference_body_md5_empty":digest.is_some_and(str::is_empty),
        "reference_body_escaped_xml":body.trim_start().starts_with("&lt;"),
        "reference_body_has_actor_envelope":envelope_body!=body,
        "reference_body_envelope_image_md5":image_digest(envelope_body).is_some(),
        "reference_body_unescaped_image_md5":unescaped.as_ref().is_some_and(|body|image_digest(body).is_some()),
        "reference_body_short_preview":body.len()<=256 && !body.contains(['<','>','&']),
        "reference_body_bracketed_preview":body.trim().starts_with('[') && body.trim().ends_with(']')})
}

#[cfg(test)]
pub fn reference_diagnostics(
    group: &str,
    row: &Value,
    raw: &Value,
    wxid: &str,
    reference: &Value,
) -> Value {
    let source = source_fields(group, row, raw, wxid);
    let failure = source
        .as_ref()
        .err()
        .map(|error| {
            let message = error.to_string();
            if message.contains("账号或群") {
                "identity"
            } else if message.contains("已撤回") {
                "recalled"
            } else if message.contains("定位")
                || message.contains("游标")
                || message.contains("分片")
            {
                "locator"
            } else if message.contains("发送者") {
                "actor"
            } else if message.contains("服务端编号") {
                "server_id"
            } else if message.contains("时间") {
                "time"
            } else if message.contains("原文") || message.contains("显示内容") {
                "body"
            } else {
                "other"
            }
        })
        .unwrap_or("none");
    let actor = if row["sender_id"] == 2 {
        wxid
    } else {
        text(row, "sender_username")
    };
    let body = db_text(&raw["content"]).or_else(|_| db_text(&raw["compress_content"]));
    let body = body
        .as_ref()
        .map(|body| remove_envelope(body, actor))
        .unwrap_or("");
    let digest = image_digest(body);
    let reference_digest = image_digest(text(reference, "text"));
    json!({"row_image":row["type"]=="图片","raw_type_matches":raw["local_type"]==reference["type"],
        "source_fields_ok":source.is_ok(),"source_failure":failure,
        "source_server_matches":source.as_ref().is_ok_and(|value| value["server_id"]==reference["server_id"]),
        "source_from_matches":source.as_ref().is_ok_and(|value| value["fromusr"]==reference["fromusr"]),
        "source_chat_matches":source.as_ref().is_ok_and(|value| value["chatusr"]==reference["chatusr"]),
        "reference_from_is_group":reference["fromusr"]==group,"reference_chat_is_group":reference["chatusr"]==group,
        "reference_from_is_actor":reference["fromusr"]==actor,"reference_chat_is_actor":reference["chatusr"]==actor,
        "actor_is_self":actor==wxid,"body_exact":xml_newlines(body)==xml_newlines(text(reference,"text")),
        "db_image_md5_present":digest.is_some(),"reference_image_md5_present":reference_digest.is_some(),
        "image_md5_matches":digest.as_ref().is_some_and(|digest| Some(digest)==reference_digest.as_ref())})
}

fn source_fields(group: &str, row: &Value, raw: &Value, wxid: &str) -> Result<Value> {
    ensure!(group_id(group) && member_id(wxid), "引用账号或群编号无效");
    ensure!(
        row["recalled"] != true && raw["recalled"] != true && row["status"] != "recalled",
        "引用原消息已撤回"
    );
    for field in ["local_id", "sort_seq", "shard", "create_time", "sender_id"] {
        ensure!(
            !row[field].is_null() && row[field] == raw[field],
            "引用原消息{field}定位已变化"
        );
    }
    for value in [row, raw] {
        if let Some(bound) = value.get("group") {
            ensure!(bound == group, "引用来源不属于当前群");
        }
    }
    let local = positive_u32(&raw["local_id"], "本地编号")?;
    let time = positive_u32(&raw["create_time"], "时间")?;
    ensure!(
        time as u64 <= SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
        "引用原消息时间尚未确认"
    );
    ensure!(row["sort_seq"].as_i64().is_some(), "引用原消息游标无效");
    let shard = text(row, "shard");
    ensure!(
        !shard.is_empty() && shard.len() <= 256 && !shard.contains(['/', '\\', '\0']),
        "引用原消息分片无效"
    );
    let server = server_id(&raw["server_id"])?;
    if let Some(value) = row.get("server_id") {
        ensure!(server_id(value)? == server, "引用原消息服务端编号已变化");
    }
    let own = row["sender_id"] == 2;
    let actor = if own {
        ensure!(
            [row, raw]
                .iter()
                .all(|value| text(value, "sender_username").is_empty()
                    || text(value, "sender_username") == wxid),
            "引用发送者与当前账号不一致"
        );
        wxid
    } else {
        let actor = text(row, "sender_username");
        ensure!(
            member_id(actor) && raw["sender_username"] == actor,
            "无法由本机数据库确认引用发送者，未相信正文中的账号前缀"
        );
        actor
    };
    let normalized = row["content"].as_str().context("引用原消息显示内容无效")?;
    let raw_body = match db_text(&raw["content"]) {
        Ok(value) if !value.trim().is_empty() => value,
        _ => db_text(&raw["compress_content"]).context("引用原文字段不能完整解码")?,
    };
    let body = remove_envelope(&raw_body, actor);
    ensure!(bounded_text(body), "引用原文为空、过长或包含NUL");
    ensure!(
        remove_envelope(normalized, actor).trim() == body.trim(),
        "引用原文与本机数据库显示记录不一致"
    );
    Ok(
        json!({"group":group,"type":raw["local_type"],"server_id":server,"local_id":local,"create_time":time,
        "fromusr":if own {wxid} else {group},"tousr":if own {group} else {wxid},"chatusr":actor,"content":body}),
    )
}

fn validate_expected(group: &str, expected: &Value) -> Result<()> {
    ensure!(
        text(expected, "kind") == "quote" && group_id(group),
        "引用发送步骤无效"
    );
    let source = &expected["source"];
    ensure!(
        source["group"] == group && source["type"] == 1,
        "引用来源不属于当前群或尚不支持该类型"
    );
    ensure!(
        server_id(&source["server_id"])? == text(source, "server_id"),
        "引用服务端编号格式无效"
    );
    positive_u32(&source["local_id"], "本地编号")?;
    positive_u32(&source["create_time"], "时间")?;
    ensure!(
        bounded_text(text(expected, "text")) && bounded_text(text(source, "content")),
        "引用原文或回复超过原生容量"
    );
    ensure!(member_id(text(source, "chatusr")), "引用发送者编号无效");
    let at = text(expected, "atlist");
    ensure!(
        at.len() <= 4096 && (at.is_empty() || at.split(',').all(member_id)),
        "引用@列表无效"
    );
    Ok(())
}
fn quote_health(value: &Value, pid: Option<u32>, enabled: bool) -> bool {
    pid.is_some_and(|pid| value["pid"].as_u64() == Some(pid as u64))
        && value["version"] == "4.1.15.13"
        && value["error"].as_str().is_none_or(str::is_empty)
        && (value["error"].is_null() || value["error"] == "")
        && value["hooks_installed"] == false
        && (!enabled || value["enabled"] == true)
        && value["capabilities"]
            .as_array()
            .is_some_and(|caps| caps.iter().any(|cap| cap == "quote_text"))
        && value["source_types"]
            .as_array()
            .is_some_and(|types| types.iter().any(|kind| kind == 1))
}
fn valid_receipt(value: &Value, group: &str, id: &str) -> bool {
    value["group"] == group
        && value["request_id"] == id
        && matches!(text(value, "status"), "queued" | "uncertain")
        && server_id(&value["source_server_id"]).is_ok()
        && value.get("uuid").is_some_and(|value| {
            value.as_str().is_some_and(|value| {
                value.len() == 36 && uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil())
            })
        })
}
fn scalar(node: roxmltree::Node<'_, '_>, tag: &str) -> Option<String> {
    let mut fields = node.children().filter(|child| child.has_tag_name(tag));
    let field = fields.next()?;
    if fields.next().is_some() || field.children().any(|child| child.is_element()) {
        return None;
    }
    Some(
        field
            .children()
            .filter(|child| child.is_text())
            .filter_map(|child| child.text())
            .collect(),
    )
}

struct NativeCard {
    reply: String,
    source: Value,
}
fn parse_card(content: &str) -> Option<NativeCard> {
    if content.len() > 2 * 1024 * 1024
        || content.contains("<!DOCTYPE")
        || content.contains("<!ENTITY")
    {
        return None;
    }
    let doc = roxmltree::Document::parse(content).ok()?;
    let root = doc.root_element();
    let app = if root.has_tag_name("appmsg") {
        root
    } else if root.has_tag_name("msg") {
        let mut apps = root.children().filter(|node| node.has_tag_name("appmsg"));
        let app = apps.next()?;
        if apps.next().is_some() {
            return None;
        }
        app
    } else {
        return None;
    };
    if scalar(app, "type").as_deref() != Some("57") {
        return None;
    }
    let reply = scalar(app, "title")?;
    let mut sources = app.children().filter(|node| node.has_tag_name("refermsg"));
    let reference = sources.next()?;
    if sources.next().is_some() {
        return None;
    }
    let id = scalar(reference, "svrid")?;
    if server_id(&json!(id)).ok().as_deref() != Some(id.as_str()) {
        return None;
    }
    let kind = scalar(reference, "type")?;
    let number = kind.parse::<u32>().ok().filter(|number| *number > 0)?;
    if number.to_string() != kind {
        return None;
    }
    let from = scalar(reference, "fromusr")?;
    let chat = scalar(reference, "chatusr")?;
    let mut names = reference
        .descendants()
        .filter(|node| node.has_tag_name("displayname"));
    let sender = match names.next() {
        None => String::new(),
        Some(name) if name.parent() == Some(reference) && names.next().is_none() => {
            scalar(reference, "displayname")?
        }
        Some(_) => return None,
    };
    let body = scalar(reference, "content")?;
    if from.len() > 128 || chat.len() > 128 || sender.len() > 1024 {
        return None;
    }
    Some(NativeCard {
        reply,
        source: json!({"server_id":id,"fromusr":from,"chatusr":chat,
        "sender":sender,"text":body,"type":number}),
    })
}

/// Parse a received native type-57 reference. `type` is an integer; a missing DB source must remain unavailable.
pub fn reference(content: &str) -> Option<Value> {
    parse_card(content).map(|card| card.source)
}

fn xml_newlines(value: &str) -> Cow<'_, str> {
    if value.contains('\r') {
        Cow::Owned(value.replace("\r\n", "\n").replace('\r', "\n"))
    } else {
        Cow::Borrowed(value)
    }
}

/// Match the actual native appmsg card, never a plain-text summary or a nested lookalike.
/// Mention metadata and the new nonzero server_id are checked separately by the DB confirmation path.
pub fn matches(content: &str, expected: &Value) -> bool {
    if validate_expected(text(&expected["source"], "group"), expected).is_err() {
        return false;
    }
    let Some(card) = parse_card(content) else {
        return false;
    };
    let source = &expected["source"];
    xml_newlines(&card.reply) == xml_newlines(text(expected, "text"))
        && card.source["type"] == 1
        && card.source["server_id"] == source["server_id"]
        && card.source["fromusr"] == source["fromusr"]
        && card.source["chatusr"] == source["chatusr"]
        && xml_newlines(text(&card.source, "text")) == xml_newlines(text(source, "content"))
}

#[cfg(test)]
mod tests {
    use super::*;
    const GROUP: &str = "12345678901@chatroom";
    const SELF: &str = "wxid_fixture_self";
    fn rows(own: bool, content: &str) -> (Value, Value) {
        let sender = if own { 2 } else { 7 };
        let actor = if own { "" } else { "wxid_member" };
        let row = json!({"local_id":7,"sort_seq":99,"shard":"message__message_0.db","create_time":1000,
            "sender_id":sender,"sender_username":actor,"type":"文本","content":content});
        let mut raw = row.clone();
        raw.as_object_mut().unwrap().remove("type");
        raw["local_type"] = json!(1);
        raw["server_id"] = json!(42);
        (row, raw)
    }
    fn expected() -> Value {
        let (row, raw) = rows(true, "原文<&> 中文");
        json!({"kind":"quote","text":"回复<&> 中文","atlist":"","source":prepare_source(GROUP,&row,&raw,SELF).unwrap()})
    }
    fn card(expected: &Value, display: &str) -> String {
        fn escape(value: &str) -> String {
            value
                .replace('&', "&amp;")
                .replace('<', "&lt;")
                .replace('>', "&gt;")
        }
        let source = &expected["source"];
        format!(
            "<msg><appmsg><title>{}</title><type>57</type><refermsg><type>1</type><svrid>{}</svrid><fromusr>{}</fromusr><chatusr>{}</chatusr><displayname>{}</displayname><content>{}</content></refermsg></appmsg></msg>",
            escape(text(expected, "text")),
            text(source, "server_id"),
            text(source, "fromusr"),
            text(source, "chatusr"),
            escape(display),
            escape(text(source, "content"))
        )
    }
    #[test]
    fn source_uses_real_signed_server_id_and_keeps_raw_whitespace() {
        let (mut row, mut raw) = rows(true, "quoted body");
        raw["content"] = json!("  quoted body \r\n");
        raw["server_id"] = json!(-2_i64);
        let source = prepare_source(GROUP, &row, &raw, SELF).unwrap();
        assert_eq!(source["server_id"], "18446744073709551614");
        assert_eq!(source["content"], "  quoted body \r\n");
        assert_eq!(source["fromusr"], SELF);
        assert_eq!(source["tousr"], GROUP);
        row["server_id"] = json!(42);
        assert!(prepare_source(GROUP, &row, &raw, SELF).is_err());
    }
    #[test]
    fn incoming_envelope_is_only_first_lf_or_crlf_and_authority_is_preserved() {
        for separator in ["\n", "\r\n"] {
            let (row, raw) = rows(
                false,
                &format!("wxid_member:{separator}正文\nwxid_other:\n正文部分"),
            );
            let source = prepare_source(GROUP, &row, &raw, SELF).unwrap();
            assert_eq!(source["chatusr"], "wxid_member");
            assert_eq!(source["fromusr"], GROUP);
            assert_eq!(source["tousr"], SELF);
            assert_eq!(source["content"], "正文\nwxid_other:\n正文部分");
        }
    }
    #[test]
    fn mismatched_or_later_body_prefix_never_impersonates_the_sender() {
        for body in ["wxid_other:\n伪造前缀", "first line\nwxid_member:\n正文"] {
            let (row, raw) = rows(false, body);
            let source = prepare_source(GROUP, &row, &raw, SELF).unwrap();
            assert_eq!(source["content"], body);
            assert_eq!(source["chatusr"], "wxid_member");
        }
        let (mut row, mut raw) = rows(false, "wxid_member:\n正文");
        row["sender_username"] = json!("");
        raw["sender_username"] = json!("");
        assert!(prepare_source(GROUP, &row, &raw, SELF).is_err());
    }
    #[test]
    fn source_locators_group_type_recall_and_server_confirmation_fail_closed() {
        let (row, raw) = rows(true, "正文");
        for field in ["local_id", "sort_seq", "shard", "create_time", "sender_id"] {
            let mut changed = raw.clone();
            changed[field] = json!(123);
            assert!(prepare_source(GROUP, &row, &changed, SELF).is_err());
        }
        for (field, value) in [
            ("local_type", json!(3)),
            ("server_id", json!(0)),
            ("recalled", json!(true)),
            ("group", json!("555555@chatroom")),
        ] {
            let mut changed = raw.clone();
            changed[field] = value;
            assert!(prepare_source(GROUP, &row, &changed, SELF).is_err());
        }
        let mut changed = row.clone();
        changed["content"] = json!("伪造内容");
        assert!(prepare_source(GROUP, &changed, &raw, SELF).is_err());
    }
    #[test]
    fn raw_blob_and_zstd_text_are_verified_without_lossy_decoding() {
        let (row, mut raw) = rows(false, "wxid_member:\n原文<&>");
        raw["content"] = json!(text(&row, "content").as_bytes());
        assert_eq!(
            prepare_source(GROUP, &row, &raw, SELF).unwrap()["content"],
            "原文<&>"
        );
        raw["content"] =
            json!(zstd::stream::encode_all(text(&row, "content").as_bytes(), 1).unwrap());
        assert_eq!(
            prepare_source(GROUP, &row, &raw, SELF).unwrap()["content"],
            "原文<&>"
        );
        raw["content"] = json!([255, 256]);
        assert!(prepare_source(GROUP, &row, &raw, SELF).is_err());
        let body = "汉".repeat(7900);
        let (row, raw) = rows(true, &body);
        let source = prepare_source(GROUP, &row, &raw, SELF).unwrap();
        let maximum = json!({"kind":"quote","text":body,"atlist":"","source":source});
        assert!(validate_expected(GROUP, &maximum).is_ok());
        let actor = "a".repeat(128);
        let body = "x".repeat(MAX_TEXT);
        let (mut row, mut raw) = rows(false, &format!("{actor}:\r\n{body}"));
        row["sender_username"] = json!(actor);
        raw["sender_username"] = row["sender_username"].clone();
        assert_eq!(
            prepare_source(GROUP, &row, &raw, SELF).unwrap()["content"],
            body
        );
        raw["content"] = json!("x".repeat(MAX_DB_TEXT + 1));
        assert!(prepare_source(GROUP, &row, &raw, SELF).is_err());
    }
    #[test]
    fn real_native_card_matches_while_contact_displayname_can_change() {
        let expected = expected();
        assert!(matches(&card(&expected, "旧昵称"), &expected));
        assert!(matches(&card(&expected, "新群昵称"), &expected));
        assert!(!matches("【引用 昵称】\n原文\n回复", &expected));
        let empty = card(&expected, "");
        assert!(matches(&empty, &expected));
        assert_eq!(reference(&empty).unwrap()["sender"], "");
        let missing = empty.replace("<displayname></displayname>", "");
        assert!(matches(&missing, &expected));
        assert_eq!(reference(&missing).unwrap()["sender"], "");
        for malicious in [
            "<displayname/><displayname>spoofed</displayname>",
            "<displayname><nested>spoofed</nested></displayname>",
            "<wrapper><displayname>spoofed</displayname></wrapper>",
        ] {
            assert!(!matches(
                &empty.replace("<displayname></displayname>", malicious),
                &expected
            ));
        }
    }
    #[test]
    fn xml_eol_rules_apply_only_to_comparison_without_trimming_or_altering_wire_text() {
        let mut expected = expected();
        expected["text"] = json!("  reply1\r\nreply2\rreply3\n尾巴 ");
        expected["source"]["content"] = json!(" 原文1\r\n原文2\r原文3\n  end ");
        let before = expected.clone();
        let value = card(&expected, "")
            .replace(
                "  reply1\r\nreply2\rreply3\n尾巴 ",
                "<![CDATA[  reply1\r\nreply2\rreply3\n尾巴 ]]>",
            )
            .replace(
                " 原文1\r\n原文2\r原文3\n  end ",
                "<![CDATA[ 原文1\r\n原文2\r原文3\n  end ]]>",
            );
        assert!(matches(&value, &expected));
        assert_eq!(expected, before);
        let mut changed = expected.clone();
        changed["text"] = json!(text(&expected, "text").trim());
        assert!(!matches(&value, &changed));
        changed = expected.clone();
        changed["source"]["content"] = json!(text(&expected["source"], "content").trim());
        assert!(!matches(&value, &changed));
        changed = expected.clone();
        changed["text"] = json!(text(&expected, "text").replace("reply1", "ｒｅｐｌｙ１"));
        assert!(!matches(&value, &changed));
    }
    #[test]
    fn native_card_requires_exact_source_reply_and_type_not_a_nested_lookalike() {
        let expected = expected();
        let original = card(&expected, "成员");
        for (from, to) in [
            ("<type>57</type>", "<type>6</type>"),
            ("<svrid>42</svrid>", "<svrid>43</svrid>"),
            (
                "<fromusr>wxid_fixture_self</fromusr>",
                "<fromusr>wxid_other</fromusr>",
            ),
            (
                "<chatusr>wxid_fixture_self</chatusr>",
                "<chatusr>wxid_other</chatusr>",
            ),
            ("回复&lt;&amp;&gt; 中文", "another reply"),
            ("原文&lt;&amp;&gt; 中文", "another original"),
        ] {
            assert!(!matches(&original.replace(from, to), &expected), "{from}");
        }
        assert!(!matches(
            &format!("<msg><wrapper>{}</wrapper></msg>", original),
            &expected
        ));
        assert!(!matches(
            &original.replace("<type>57</type>", "<type>57</type><type>57</type>"),
            &expected
        ));
        assert!(!matches(
            &original.replace("<refermsg>", "<refermsg><content>fake</content>"),
            &expected
        ));
    }
    #[test]
    fn split_cdata_is_combined_and_external_entities_are_rejected() {
        let mut expected = expected();
        expected["text"] = json!("reply]]>tail");
        let value = card(&expected, "成员")
            .replace("reply]]&gt;tail", "<![CDATA[reply]]]]><![CDATA[>tail]]>");
        assert!(matches(&value, &expected));
        assert!(!matches(
            &format!("<!DOCTYPE msg [<!ENTITY x SYSTEM 'file:///private'>]>{value}"),
            &expected
        ));
    }
    #[test]
    fn received_reference_has_one_direct_scope_and_a_real_server_identifier() {
        let value = card(&expected(), "成员");
        let source = reference(&value).unwrap();
        assert_eq!(
            source,
            json!({"type":1,"server_id":"42","fromusr":SELF,"chatusr":SELF,"sender":"成员","text":"原文<&> 中文"})
        );
        for id in ["0", "-1", "00042", "18446744073709551616"] {
            assert!(
                reference(&value.replace("<svrid>42</svrid>", &format!("<svrid>{id}</svrid>")))
                    .is_none()
            );
        }
        assert!(
            reference(&value.replace(
                "<type>57</type>",
                "<type>6</type><nested><type>57</type></nested>"
            ))
            .is_none()
        );
        assert!(reference(&value.replace("<refermsg>", "<refermsg><type>1</type>")).is_none());
    }
    #[test]
    fn image_reference_resolution_keeps_source_identity_checks_and_text_send_restriction() {
        let body = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
        let (mut row, mut raw) = rows(false, body);
        row["type"] = json!("图片");
        raw["local_type"] = json!(3);
        let reference =
            json!({"type":3,"server_id":"42","fromusr":GROUP,"chatusr":"wxid_member","text":body});
        assert!(matches_reference(GROUP, &row, &raw, SELF, &reference));
        assert!(prepare_source(GROUP, &row, &raw, SELF).is_err());
        for (field, value) in [
            ("type", json!(1)),
            ("server_id", json!("43")),
            ("fromusr", json!("555555@chatroom")),
            ("chatusr", json!(SELF)),
            ("text", json!("other image")),
        ] {
            let mut changed = reference.clone();
            changed[field] = value;
            assert!(
                !matches_reference(GROUP, &row, &raw, SELF, &changed),
                "{field}"
            );
        }
        for (field, value) in [
            ("status", json!("recalled")),
            ("sender_id", json!(9)),
            ("group", json!("555555@chatroom")),
            ("content", json!("different body")),
        ] {
            let mut changed = row.clone();
            changed[field] = value;
            assert!(
                !matches_reference(GROUP, &changed, &raw, SELF, &reference),
                "{field}"
            );
        }
    }
    #[test]
    fn received_image_reference_uses_verified_media_identity_instead_of_transport_xml() {
        let body = "<?xml version='1.0'?><msg><img md5='0123456789abcdef0123456789abcdef' cdnmidimgurl='old-download' length='123'/></msg>";
        let (mut row, mut raw) = rows(false, body);
        row["type"] = json!("图片");
        raw["local_type"] = json!(3);
        let reference = json!({"type":3,"server_id":"42","fromusr":GROUP,"chatusr":"wxid_member",
            "text":"<msg>\n<img length='123' cdnmidimgurl='new-download' md5='0123456789ABCDEF0123456789ABCDEF'/>\n</msg>"});
        assert!(matches_reference(GROUP, &row, &raw, SELF, &reference));
        for body in [
            "<msg><img md5='fedcba9876543210fedcba9876543210'/></msg>",
            "<msg><img cdnthumbaeskey='fixture-not-a-media-identifier'/></msg>",
            "<msg><img md5='invalid'/></msg>",
            "<msg><wrapper><img md5='0123456789abcdef0123456789abcdef'/></wrapper></msg>",
            "<msg><img md5='0123456789abcdef0123456789abcdef'/><img md5='0123456789abcdef0123456789abcdef'/></msg>",
            "<!DOCTYPE msg [<!ENTITY x 'ignored'>]><msg><img md5='0123456789abcdef0123456789abcdef'/></msg>",
        ] {
            let mut changed = reference.clone();
            changed["text"] = json!(body);
            assert!(!matches_reference(GROUP, &row, &raw, SELF, &changed));
        }
    }
    #[test]
    fn received_reference_sender_fields_bind_to_the_database_actor_and_group() {
        let body = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
        for own in [false, true] {
            let (mut row, mut raw) = rows(own, body);
            row["type"] = json!("图片");
            raw["local_type"] = json!(3);
            let actor = if own { SELF } else { "wxid_member" };
            for from in [actor, GROUP] {
                let reference =
                    json!({"type":3,"server_id":"42","fromusr":from,"chatusr":actor,"text":body});
                assert!(matches_reference(GROUP, &row, &raw, SELF, &reference));
                for (field, value) in [
                    ("fromusr", "wxid_third_party"),
                    ("chatusr", "wxid_third_party"),
                ] {
                    let mut changed = reference.clone();
                    changed[field] = json!(value);
                    assert!(!matches_reference(GROUP, &row, &raw, SELF, &changed));
                }
            }
        }
    }
    #[test]
    fn received_image_reference_strips_only_the_verified_database_actor_envelope() {
        let body = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
        let (mut row, mut raw) = rows(false, body);
        row["type"] = json!("图片");
        raw["local_type"] = json!(3);
        let reference = json!({"type":3,"server_id":"42","fromusr":GROUP,"chatusr":"wxid_member","text":format!("wxid_member:\r\n{body}")});
        assert!(matches_reference(GROUP, &row, &raw, SELF, &reference));
        for content in [
            format!("wxid_other:\n{body}"),
            format!("prefix\nwxid_member:\n{body}"),
            "wxid_member:\n<msg><img md5='fedcba9876543210fedcba9876543210'/></msg>".into(),
        ] {
            let mut changed = reference.clone();
            changed["text"] = json!(content);
            assert!(!matches_reference(GROUP, &row, &raw, SELF, &changed));
        }
    }
    #[test]
    fn receipt_and_health_cannot_confuse_actions_a_different_task_or_missing_uuid() {
        let id = request_id("fixture-key");
        let mut value = json!({"group":GROUP,"request_id":id,"status":"queued","source_server_id":"42","uuid":"00000000-0000-4000-8000-000000000001"});
        assert!(valid_receipt(&value, GROUP, &id));
        value["status"] = json!("uncertain");
        assert!(valid_receipt(&value, GROUP, &id));
        assert!(!valid_receipt(&value, "555555@chatroom", &id));
        value["uuid"] = json!("");
        assert!(!valid_receipt(&value, GROUP, &id));
        let mut health = json!({"pid":7,"version":"4.1.15.13","enabled":true,"hooks_installed":false,"capabilities":["quote_text"],"source_types":[1]});
        assert!(quote_health(&health, Some(7), true));
        health["capabilities"] = json!(["recall"]);
        assert!(!quote_health(&health, Some(7), true));
    }
    #[test]
    fn missing_optional_dll_still_clears_old_quote_grants_without_touching_actions() {
        let temp = tempfile::tempdir().unwrap();
        let quote = temp.path().join("native-quotes/old/native.json");
        let actions = temp.path().join("native-actions/old/native.json");
        for path in [&quote, &actions] {
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(
                path,
                json!({"groups":[GROUP],"token":"a".repeat(64),"port":1}).to_string(),
            )
            .unwrap();
        }
        let instance = Quotes::new(temp.path().to_owned(), SELF);
        drop(instance);
        let quote: Value = serde_json::from_slice(&std::fs::read(quote).unwrap()).unwrap();
        let actions: Value = serde_json::from_slice(&std::fs::read(actions).unwrap()).unwrap();
        assert_eq!(quote["groups"], json!([]));
        assert_eq!(actions["groups"], json!([GROUP]));
    }
    #[test]
    fn restrict_can_only_intersect_existing_grants_without_loading_a_component() {
        let temp = tempfile::tempdir().unwrap();
        let config = temp.path().join("native-quotes/old/native.json");
        std::fs::create_dir_all(config.parent().unwrap()).unwrap();
        let other = "555555@chatroom".to_owned();
        std::fs::write(
            &config,
            json!({"groups":[GROUP,other],"token":"a".repeat(64),"port":1}).to_string(),
        )
        .unwrap();
        let mut quotes = Quotes {
            client: NativeClient::new(temp.path().join("native-quotes")),
            directory: temp.path().to_owned(),
            connected: false,
            allowed: BTreeSet::from([GROUP.to_owned()]),
        };
        quotes
            .restrict(&BTreeSet::from([GROUP.to_owned(), other]))
            .unwrap();
        let value: Value = serde_json::from_slice(&std::fs::read(config).unwrap()).unwrap();
        assert_eq!(value["groups"], json!([GROUP]));
    }
}
