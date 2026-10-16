#[path = "db/announcement.rs"]
mod announcement;
#[path = "db/crypto.rs"]
mod crypto;
#[cfg(windows)]
#[path = "db/windows.rs"]
mod windows;

use anyhow::{Context, Result, bail, ensure};
use crypto::{Key, PAGE, SnapshotVersion};
use md5::{Digest, Md5};
use rusqlite::{
    Connection, OptionalExtension, Row, params_from_iter,
    types::{Value as SqlValue, ValueRef},
};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, HashMap, VecDeque},
    fs::File,
    io::Read,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

const NEGATIVE_LIMIT: usize = 128;

#[derive(Clone, PartialEq)]
struct PageQuery {
    sql: String,
    params: Vec<SqlValue>,
    descending: bool,
    limit: usize,
    raw: bool,
}

struct NegativePage {
    group: String,
    rel: String,
    version: SnapshotVersion,
    query: Option<PageQuery>,
}

pub struct Db {
    pub wxid: String,
    pub account: String,
    pub account_dir: PathBuf,
    pub cfg_dword: Option<u32>,
    files: BTreeMap<String, PathBuf>,
    keys: BTreeMap<String, Key>,
    connections: HashMap<String, Connection>,
    snapshot_versions: HashMap<String, SnapshotVersion>,
    negative_pages: VecDeque<NegativePage>,
    names: HashMap<String, HashMap<String, String>>,
    last_catalog: Instant,
    last_keys: Instant,
}

impl Db {
    #[cfg(test)]
    pub(crate) fn media_fixture(
        account_dir: PathBuf,
        wxid: &str,
        cfg_dword: Option<u32>,
        connections: HashMap<String, Connection>,
    ) -> Self {
        let files = connections
            .keys()
            .map(|rel| (rel.clone(), account_dir.join("db_storage").join(rel)))
            .collect();
        Self {
            wxid: wxid.into(),
            account: wxid.into(),
            account_dir,
            cfg_dword,
            files,
            keys: BTreeMap::new(),
            connections,
            snapshot_versions: HashMap::new(),
            negative_pages: VecDeque::new(),
            names: HashMap::new(),
            last_catalog: Instant::now(),
            last_keys: Instant::now(),
        }
    }
    pub fn connect(directory: &Path) -> Result<Self> {
        #[cfg(not(windows))]
        {
            let _ = directory;
            bail!("本机微信数据库适配仅支持 Windows");
        }
        #[cfg(windows)]
        {
            let accounts = discover_accounts(directory)?;
            let secrets = windows::extract()?;
            let mut best: Option<(
                (bool, usize, u64),
                PathBuf,
                String,
                String,
                BTreeMap<String, Key>,
                Option<u32>,
            )> = None;
            for account_dir in accounts {
                let files = collect_files(&account_dir)?;
                let mut keys = match_keys(&files, &secrets.candidates)?;
                let contact = files
                    .keys()
                    .find(|rel| {
                        Path::new(rel)
                            .file_name()
                            .is_some_and(|n| n == "contact.db")
                    })
                    .cloned();
                if let Some(contact) = &contact {
                    if !keys.contains_key(contact) {
                        for (master, _, _) in &secrets.configs {
                            if let Some(key) = derived_key(&files[contact], master)? {
                                keys.insert(contact.clone(), key);
                                for (rel, path) in &files {
                                    if !keys.contains_key(rel) {
                                        if let Some(key) = derived_key(path, master)? {
                                            keys.insert(rel.clone(), key);
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
                if !contact.as_ref().is_some_and(|rel| keys.contains_key(rel)) {
                    continue;
                }
                let account = account_dir
                    .file_name()
                    .context("微信账号目录无效")?
                    .to_string_lossy()
                    .into_owned();
                let wxid = strip_account_suffix(&account);
                let config = secrets.configs.iter().find(|(_, _, id)| *id == wxid);
                let score = (
                    config.is_some(),
                    keys.len(),
                    files
                        .values()
                        .filter_map(|p| crypto::signature(p).ok().flatten().map(|s| s.0))
                        .max()
                        .unwrap_or(0),
                );
                if best
                    .as_ref()
                    .is_none_or(|(previous, _, _, _, _, _)| score > *previous)
                {
                    best = Some((
                        score,
                        account_dir,
                        account,
                        wxid,
                        keys,
                        config.map(|(_, dword, _)| *dword),
                    ));
                }
            }
            let (_, account_dir, account, wxid, keys, cfg_dword) =
                best.context("当前登录微信的数据库密钥未通过校验，未读取任何消息")?;
            let files = collect_files(&account_dir)?;
            Ok(Self {
                wxid,
                account,
                account_dir,
                cfg_dword,
                files,
                keys,
                connections: HashMap::new(),
                snapshot_versions: HashMap::new(),
                negative_pages: VecDeque::new(),
                names: HashMap::new(),
                last_catalog: Instant::now(),
                last_keys: Instant::now(),
            })
        }
    }

    pub fn rels(&self) -> Vec<String> {
        self.files.keys().cloned().collect()
    }
    pub fn release(&mut self) {
        self.connections.clear();
        self.snapshot_versions.clear();
        self.names.clear();
    }

    pub fn signatures(&mut self) -> Result<Vec<(String, u64, u64)>> {
        self.refresh_catalog(true)?;
        let mut result = Vec::new();
        for (_, path) in self.files.iter().filter(|(rel, _)| message_rel(rel)) {
            for path in [path.clone(), crypto::wal_path(path)?] {
                if let Some(signature) = crypto::signature(&path)? {
                    result.push((
                        path.file_name()
                            .context("消息分片名无效")?
                            .to_string_lossy()
                            .into_owned(),
                        signature.token(),
                        signature.1,
                    ));
                }
            }
        }
        result.sort();
        Ok(result)
    }

    fn refresh_catalog(&mut self, force: bool) -> Result<()> {
        if !force && self.last_catalog.elapsed() < Duration::from_secs(1) {
            return Ok(());
        }
        self.last_catalog = Instant::now();
        let current = collect_files(&self.account_dir)?;
        if current != self.files {
            self.connections.retain(|rel, _| {
                current.get(rel).is_some() && current.get(rel) == self.files.get(rel)
            });
            self.snapshot_versions.retain(|rel, _| {
                current.get(rel).is_some() && current.get(rel) == self.files.get(rel)
            });
            self.negative_pages.retain(|entry| {
                current.get(&entry.rel).is_some()
                    && current.get(&entry.rel) == self.files.get(&entry.rel)
            });
            self.keys.retain(|rel, _| {
                current.get(rel).is_some() && current.get(rel) == self.files.get(rel)
            });
            self.files = current;
        }
        Ok(())
    }

    pub fn open(&mut self, rel: &str) -> Result<&Connection> {
        let rel = rel.replace('\\', "/");
        if !self.connections.contains_key(&rel) {
            self.refresh_catalog(false)?;
            let path = self
                .files
                .get(&rel)
                .context("微信数据库分片不存在")?
                .clone();
            if !self.keys.contains_key(&rel) {
                #[cfg(windows)]
                if self.last_keys.elapsed() >= Duration::from_secs(5) {
                    self.last_keys = Instant::now();
                    let secrets = windows::extract()?;
                    self.keys
                        .extend(match_keys(&self.files, &secrets.candidates)?);
                    if !self.keys.contains_key(&rel) {
                        for (master, _, _) in &secrets.configs {
                            if let Some(key) = derived_key(&path, master)? {
                                self.keys.insert(rel.clone(), key);
                                break;
                            }
                        }
                    }
                }
            }
            let key = self
                .keys
                .get(&rel)
                .context("微信新分片密钥尚不可用，本轮未推进游标")?;
            let (connection, version) = crypto::snapshot(&path, key)?;
            self.snapshot_versions.insert(rel.clone(), version);
            self.connections.insert(rel.clone(), connection);
        }
        Ok(self.connections.get(&rel).expect("inserted snapshot"))
    }

    fn contact_rel(&self) -> Result<String> {
        self.files
            .keys()
            .find(|rel| {
                Path::new(rel)
                    .file_name()
                    .is_some_and(|n| n == "contact.db")
            })
            .cloned()
            .context("微信联系人数据库不存在")
    }

    pub fn groups(&mut self) -> Result<Vec<Value>> {
        let rel = self.contact_rel()?;
        let connection = self.open(&rel)?;
        let mut statement=connection.prepare("SELECT c.username,coalesce(nullif(c.remark,''),nullif(c.nick_name,''),c.username),coalesce(r.owner,''),count(mc.username) FROM contact c LEFT JOIN chat_room r ON r.username=c.username LEFT JOIN chatroom_member m ON m.room_id=r.id LEFT JOIN contact mc ON mc.id=m.member_id WHERE c.username LIKE '%@chatroom' AND c.is_in_chat_room=1 AND c.delete_flag=0 GROUP BY c.id ORDER BY c.id")?;
        statement.query_map([],|r|Ok(json!({"username":string(r,0)?,"name":string(r,1)?,"owner":string(r,2)?,"member_count":r.get::<_,i64>(3)?})))?.collect::<rusqlite::Result<_>>().map_err(Into::into)
    }

    pub fn members(&mut self, group: &str) -> Result<Vec<Value>> {
        let rel = self.contact_rel()?;
        let connection = self.open(&rel)?;
        let mut statement=connection.prepare("SELECT c.username,c.nick_name,c.remark,c.username=r.owner FROM chat_room r JOIN chatroom_member m ON m.room_id=r.id JOIN contact c ON c.id=m.member_id WHERE r.username=?1 ORDER BY c.username")?;
        statement.query_map([group],|r|Ok(json!({"username":string(r,0)?,"nick_name":string(r,1)?,"remark":string(r,2)?,"is_owner":r.get::<_,bool>(3)?})))?.collect::<rusqlite::Result<_>>().map_err(Into::into)
    }

    pub fn announcement(&mut self, group: &str) -> Result<Value> {
        let rel = self.contact_rel()?;
        announcement::read(self.open(&rel)?, group)
    }

    pub fn display_name(&mut self, group: &str, id: &str) -> Result<String> {
        let load_names = group.ends_with("@chatroom") && !self.names.contains_key(group);
        let rel = self.contact_rel()?;
        let connection = self.open(&rel)?;
        let contact: Option<(String, String)> = connection
            .query_row(
                "SELECT remark,nick_name FROM contact WHERE username=?1 LIMIT 1",
                [id],
                |r| Ok((string(r, 0)?, string(r, 1)?)),
            )
            .optional()?;
        if let Some((remark, _)) = &contact {
            if !remark.is_empty() {
                return Ok(remark.clone());
            }
        }
        if load_names {
            let data: Option<Vec<u8>> = connection
                .query_row(
                    "SELECT ext_buffer FROM chat_room WHERE username=?1 LIMIT 1",
                    [group],
                    |r| r.get(0),
                )
                .optional()?
                .flatten();
            let names = data
                .and_then(|bytes| room_names(&bytes).ok())
                .unwrap_or_default();
            self.names.insert(group.into(), names);
        }
        Ok(self
            .names
            .get(group)
            .and_then(|names| names.get(id))
            .cloned()
            .or_else(|| contact.map(|(_, nick)| nick).filter(|s| !s.is_empty()))
            .unwrap_or_else(|| id.into()))
    }

    pub fn avatar(&mut self, id: &str) -> Result<Option<Vec<u8>>> {
        let Some(rel) = self
            .files
            .keys()
            .find(|rel| {
                Path::new(rel)
                    .file_name()
                    .is_some_and(|n| n == "head_image.db")
            })
            .cloned()
        else {
            return Ok(None);
        };
        self.open(&rel)?.query_row("SELECT image_buffer FROM head_image WHERE username=?1 AND length(image_buffer) BETWEEN 1 AND 2097152 LIMIT 1",[id],|r|r.get(0)).optional().map_err(Into::into)
    }

    fn message_rels(&mut self) -> Result<Vec<String>> {
        self.refresh_catalog(false)?;
        Ok(self
            .files
            .keys()
            .filter(|rel| message_rel(rel))
            .cloned()
            .collect())
    }

    fn rows(
        &mut self,
        group: &str,
        rel: &str,
        where_sql: &str,
        params: Vec<SqlValue>,
        descending: bool,
        limit: usize,
        raw: bool,
    ) -> Result<Vec<Value>> {
        let query = PageQuery {
            sql: where_sql.into(),
            params: params.clone(),
            descending,
            limit: limit.min(10000),
            raw,
        };
        let path = self.files.get(rel).cloned();
        let before = path
            .as_ref()
            .and_then(|path| crypto::version(path).ok().flatten());
        if let Some(version) = &before {
            let matches_connection = !self.connections.contains_key(rel)
                || self.snapshot_versions.get(rel) == Some(version);
            if matches_connection
                && self.negative_pages.iter().any(|entry| {
                    entry.rel == rel
                        && entry.group == group
                        && entry.version == *version
                        && entry.query.as_ref().is_none_or(|cached| *cached == query)
                })
                && path
                    .as_ref()
                    .and_then(|path| crypto::version(path).ok().flatten())
                    .as_ref()
                    == Some(version)
            {
                return Ok(Vec::new());
            }
        }
        let (rows, missing_table) =
            self.query_rows(group, rel, where_sql, params, descending, limit, raw)?;
        if rows.is_empty()
            && let Some(version) = before
            && self.snapshot_versions.get(rel) == Some(&version)
            && path
                .as_ref()
                .and_then(|path| crypto::version(path).ok().flatten())
                .as_ref()
                == Some(&version)
        {
            self.negative_pages.retain(|entry| {
                entry.rel != rel
                    || entry.group != group
                    || (entry.version == version
                        && entry.query.as_ref().is_some_and(|cached| *cached != query))
            });
            self.negative_pages.push_back(NegativePage {
                group: group.into(),
                rel: rel.into(),
                version,
                query: if missing_table { None } else { Some(query) },
            });
            while self.negative_pages.len() > NEGATIVE_LIMIT {
                self.negative_pages.pop_front();
            }
        }
        Ok(rows)
    }

    fn query_rows(
        &mut self,
        group: &str,
        rel: &str,
        where_sql: &str,
        params: Vec<SqlValue>,
        descending: bool,
        limit: usize,
        raw: bool,
    ) -> Result<(Vec<Value>, bool)> {
        let connection = self.open(rel)?;
        let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
        if !connection.query_row(
            "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1)",
            [&table],
            |r| r.get::<_, bool>(0),
        )? {
            return Ok((Vec::new(), true));
        }
        let fields = if raw {
            "local_id,local_type,real_sender_id,create_time,message_content,source,packed_info_data,compress_content,server_id,sort_seq"
        } else {
            "local_id,local_type,real_sender_id,create_time,message_content,source,compress_content,sort_seq"
        };
        let sql = format!(
            "SELECT {fields} FROM {table} {where_sql} ORDER BY sort_seq {0},local_id {0} LIMIT {1}",
            if descending { "DESC" } else { "ASC" },
            limit.min(10000)
        );
        let mut statement = connection.prepare(&sql)?;
        let mut cursor = statement.query(params_from_iter(params))?;
        let mut result = Vec::new();
        let shard = rel.replace('/', "__");
        while let Some(row) = cursor.next()? {
            let sender_id: i64 = row.get(2)?;
            let sender = if sender_id != 0 && sender_id != 2 {
                connection
                    .query_row(
                        "SELECT user_name FROM Name2Id WHERE rowid=?1",
                        [sender_id],
                        |r| string(r, 0),
                    )
                    .optional()?
                    .unwrap_or_default()
            } else {
                String::new()
            };
            let mut value = json!({"local_id":row.get::<_,i64>(0)?,"sender_id":sender_id,"sender_username":sender,"create_time":sql_json(row.get_ref(3)?),"source":sql_json(row.get_ref(5)?),"shard":shard});
            let kind: i64 = row.get(1)?;
            if raw {
                value["local_type"] = kind.into();
                value["server_id"] = sql_json(row.get_ref(8)?);
                value["sort_seq"] = row.get::<_, i64>(9)?.into();
                value["content"] = sql_json(row.get_ref(4)?);
                value["packed_info"] = sql_json(row.get_ref(6)?);
                value["compress_content"] = sql_json(row.get_ref(7)?);
            } else {
                let kind = message_type(kind);
                let label = kind
                    .as_str()
                    .map(str::to_owned)
                    .unwrap_or_else(|| kind.to_string());
                let mut content = friendly(row.get_ref(4)?, &label);
                if content == format!("[{label}]") {
                    let alternative = friendly(row.get_ref(6)?, &label);
                    if alternative != content {
                        content = alternative;
                    }
                }
                value["type"] = kind;
                value["content"] = content.into();
                value["sort_seq"] = row.get::<_, i64>(7)?.into();
            }
            result.push(value);
        }
        Ok((result, false))
    }

    pub fn recent(&mut self, group: &str, limit: usize) -> Result<Vec<Value>> {
        if limit == 0 {
            return Ok(Vec::new());
        }
        ensure!(limit <= 10000, "消息页过大");
        let mut rows = Vec::new();
        for rel in self.message_rels()? {
            rows.extend(self.rows(group, &rel, "", vec![], true, limit, false)?);
        }
        rows.sort_by_key(|row| std::cmp::Reverse(cursor_tuple(row)));
        rows.truncate(limit);
        Ok(rows)
    }

    pub fn page(&mut self, group: &str, position: &Value, limit: usize) -> Result<Vec<Value>> {
        if limit == 0 {
            return Ok(Vec::new());
        }
        ensure!(limit <= 10000, "消息页过大");
        let sequence = position["sort_seq"]
            .as_i64()
            .or_else(|| position.as_i64())
            .context("消息游标缺少 sort_seq")?;
        let cursor_shard = position["shard"].as_str().unwrap_or("");
        let id = position["local_id"].as_i64().unwrap_or(0);
        let mut rows = Vec::new();
        for rel in self.message_rels()? {
            let label = rel.replace('/', "__");
            let (filter, params) = match label.as_str().cmp(cursor_shard) {
                std::cmp::Ordering::Equal => (
                    "WHERE sort_seq>?1 OR (sort_seq=?1 AND local_id>?2)",
                    vec![sequence.into(), id.into()],
                ),
                std::cmp::Ordering::Greater => ("WHERE sort_seq>=?1", vec![sequence.into()]),
                std::cmp::Ordering::Less => ("WHERE sort_seq>?1", vec![sequence.into()]),
            };
            rows.extend(self.rows(group, &rel, filter, params, false, limit, false)?);
        }
        rows.sort_by_key(cursor_tuple);
        rows.truncate(limit);
        Ok(rows)
    }

    pub fn older(
        &mut self,
        group: &str,
        before: Option<&Value>,
        limit: usize,
    ) -> Result<Vec<Value>> {
        if limit == 0 {
            return Ok(Vec::new());
        }
        ensure!(limit <= 61, "历史页过大");
        let Some(before) = before else {
            return self.recent(group, limit);
        };
        let sequence = before["sort_seq"]
            .as_i64()
            .context("历史游标缺少 sort_seq")?;
        let cursor_shard = before["shard"].as_str().context("历史游标缺少分片")?;
        let id = before["local_id"]
            .as_i64()
            .context("历史游标缺少 local_id")?;
        let mut rows = Vec::new();
        for rel in self.message_rels()? {
            let label = rel.replace('/', "__");
            let (filter, params) = match label.as_str().cmp(cursor_shard) {
                std::cmp::Ordering::Equal => (
                    "WHERE sort_seq<?1 OR (sort_seq=?1 AND local_id<?2)",
                    vec![sequence.into(), id.into()],
                ),
                std::cmp::Ordering::Less => ("WHERE sort_seq<=?1", vec![sequence.into()]),
                std::cmp::Ordering::Greater => ("WHERE sort_seq<?1", vec![sequence.into()]),
            };
            rows.extend(self.rows(group, &rel, filter, params, true, limit, false)?);
        }
        rows.sort_by_key(|row| std::cmp::Reverse(cursor_tuple(row)));
        rows.truncate(limit);
        Ok(rows)
    }

    pub fn reload(&mut self, group: &str, locator: &Value) -> Result<Value> {
        let label = locator["shard"].as_str().context("消息定位缺少分片")?;
        let id = locator["local_id"]
            .as_i64()
            .context("消息定位缺少 local_id")?;
        let sequence = locator["sort_seq"]
            .as_i64()
            .context("消息定位缺少 sort_seq")?;
        let rel = self
            .rels()
            .into_iter()
            .find(|r| r.replace('/', "__") == label)
            .context("消息分片已移除")?;
        let mut rows = self.rows(
            group,
            &rel,
            "WHERE local_id=?1 AND sort_seq=?2",
            vec![id.into(), sequence.into()],
            false,
            2,
            false,
        )?;
        ensure!(rows.len() == 1, "微信原消息已删除或无法唯一读取");
        Ok(rows.remove(0))
    }
    pub fn raw_message(&mut self, group: &str, id: i64, label: &str) -> Result<Value> {
        ensure!(!label.is_empty(), "媒体消息必须包含唯一分片定位");
        let rel = self
            .rels()
            .into_iter()
            .find(|r| r.replace('/', "__") == label)
            .context("消息分片已移除")?;
        let mut rows = self.rows(
            group,
            &rel,
            "WHERE local_id=?1",
            vec![id.into()],
            false,
            2,
            true,
        )?;
        ensure!(rows.len() == 1, "微信原消息已删除或无法唯一读取");
        Ok(rows.remove(0))
    }
}

fn message_rel(rel: &str) -> bool {
    rel.strip_prefix("message/message_")
        .and_then(|s| s.strip_suffix(".db"))
        .is_some_and(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
}

fn sequence(value: &Value) -> i64 {
    value["sort_seq"].as_i64().unwrap_or(0)
}
fn local_id(value: &Value) -> i64 {
    value["local_id"].as_i64().unwrap_or(0)
}
fn shard(value: &Value) -> &str {
    value["shard"].as_str().unwrap_or("")
}
fn cursor_tuple(value: &Value) -> (i64, String, i64) {
    (sequence(value), shard(value).into(), local_id(value))
}
fn string(row: &Row<'_>, column: usize) -> rusqlite::Result<String> {
    Ok(match row.get_ref(column)? {
        ValueRef::Text(bytes) => String::from_utf8_lossy(bytes).into_owned(),
        ValueRef::Null => String::new(),
        _ => String::new(),
    })
}
fn sql_json(value: ValueRef<'_>) -> Value {
    match value {
        ValueRef::Null => Value::Null,
        ValueRef::Integer(value) => value.into(),
        ValueRef::Real(value) => json!(value),
        ValueRef::Text(bytes) => match std::str::from_utf8(bytes) {
            Ok(s) => s.into(),
            Err(_) => json!(bytes),
        },
        ValueRef::Blob(bytes) => json!(bytes),
    }
}
fn message_type(kind: i64) -> Value {
    let name = |kind| match kind {
        1 => Some("文本"),
        3 => Some("图片"),
        34 => Some("语音"),
        43 => Some("视频"),
        47 | 11000 => Some("动画表情"),
        48 => Some("位置"),
        49 => Some("文件/链接/卡片"),
        50 => Some("音视频通话"),
        10000 => Some("系统消息"),
        8594229559345 => Some("红包"),
        _ => None,
    };
    name(kind)
        .or_else(|| {
            if kind > 65535 {
                name(kind & 0xffff_ffff).or_else(|| name(kind & 0xff))
            } else {
                None
            }
        })
        .map(|s| s.into())
        .unwrap_or_else(|| kind.into())
}
fn friendly(value: ValueRef<'_>, kind: &str) -> String {
    let data = match value {
        ValueRef::Text(bytes) | ValueRef::Blob(bytes) => bytes,
        _ => return format!("[{kind}]"),
    };
    if let Ok(text) = std::str::from_utf8(data) {
        let text = text.split("\u{1}\0").next().unwrap_or(text).trim();
        if !text.is_empty() {
            return text.into();
        }
    }
    if data.starts_with(b"\x28\xb5\x2f\xfd") {
        if let Ok(mut decoder) = zstd::stream::read::Decoder::new(data) {
            let mut output = Vec::new();
            if decoder.window_log_max(23).is_ok()
                && decoder
                    .take(8 * 1024 * 1024 + 1)
                    .read_to_end(&mut output)
                    .is_ok()
                && output.len() <= 8 * 1024 * 1024
            {
                if let Ok(text) = String::from_utf8(output) {
                    if !text.trim().is_empty() {
                        return text.trim().into();
                    }
                }
            }
        }
        for offset in std::iter::once(10).chain((0..16).filter(|v| *v != 10)) {
            if let Some(bytes) = data.get(offset..) {
                let end = memchr::memmem::find(bytes, b"\x01\x00").unwrap_or(bytes.len());
                if let Ok(text) = std::str::from_utf8(&bytes[..end]) {
                    let text = text.trim();
                    let count = text.chars().count();
                    if count > 0
                        && text
                            .chars()
                            .filter(|c| c.is_control() && !matches!(c, '\n' | '\r' | '\t'))
                            .count()
                            * 10
                            <= count
                    {
                        return text.into();
                    }
                }
            }
        }
    }
    format!("[{kind}]")
}

fn protobuf_bytes(mut data: &[u8]) -> Result<Vec<(u64, &[u8])>> {
    fn varint(data: &mut &[u8]) -> Result<u64> {
        let mut value = 0;
        for shift in (0..70).step_by(7) {
            let byte = *data.first().context("微信群昵称字段截断")?;
            *data = &data[1..];
            ensure!(shift < 63 || byte < 2, "微信群昵称字段无效");
            value |= ((byte & 127) as u64) << shift;
            if byte & 128 == 0 {
                return Ok(value);
            }
        }
        bail!("微信群昵称字段无效")
    }
    ensure!(data.len() <= 8_000_000, "微信群昵称数据过大");
    let mut result = Vec::new();
    while !data.is_empty() {
        let key = varint(&mut data)?;
        ensure!(key >> 3 > 0, "微信群昵称字段无效");
        let wire = key & 7;
        let size = match wire {
            0 => {
                varint(&mut data)?;
                continue;
            }
            1 => 8,
            2 => varint(&mut data)?.try_into()?,
            5 => 4,
            _ => bail!("微信群昵称编码不支持"),
        };
        ensure!(size <= data.len(), "微信群昵称数据截断");
        if wire == 2 {
            result.push((key >> 3, &data[..size]));
        }
        data = &data[size..];
    }
    Ok(result)
}
fn room_names(data: &[u8]) -> Result<HashMap<String, String>> {
    let mut result = HashMap::new();
    for (field, member) in protobuf_bytes(data)? {
        if field != 1 {
            continue;
        }
        let values = protobuf_bytes(member)?
            .into_iter()
            .collect::<HashMap<_, _>>();
        let id = std::str::from_utf8(values.get(&1).copied().unwrap_or_default())?;
        let name = std::str::from_utf8(values.get(&2).copied().unwrap_or_default())?;
        if !id.is_empty() && !name.is_empty() {
            ensure!(
                result.insert(id.into(), name.into()).is_none(),
                "微信群昵称成员重复"
            );
        }
    }
    Ok(result)
}

fn collect_files(account: &Path) -> Result<BTreeMap<String, PathBuf>> {
    let base = account.join("db_storage");
    let mut result = BTreeMap::new();
    for entry in walkdir::WalkDir::new(&base)
        .follow_links(false)
        .into_iter()
        .filter_entry(|e| e.file_name() != "migrate")
    {
        let entry = entry?;
        if entry.file_type().is_file() && entry.path().extension().is_some_and(|v| v == "db") {
            result.insert(
                entry
                    .path()
                    .strip_prefix(&base)?
                    .to_string_lossy()
                    .replace('\\', "/"),
                entry.path().to_owned(),
            );
        }
    }
    Ok(result)
}
fn first_page(path: &Path) -> Result<[u8; PAGE]> {
    let mut page = [0; PAGE];
    File::open(path)?.read_exact(&mut page)?;
    Ok(page)
}
fn match_keys(
    files: &BTreeMap<String, PathBuf>,
    candidates: &[Vec<u8>],
) -> Result<BTreeMap<String, Key>> {
    let mut keys = BTreeMap::new();
    for (rel, path) in files {
        if let Ok(page) = first_page(path) {
            for candidate in candidates {
                if let Some(key) = Key::checked(candidate, &page) {
                    keys.insert(rel.clone(), key);
                    break;
                }
            }
        }
    }
    Ok(keys)
}
fn derived_key(path: &Path, master: &[u8; 32]) -> Result<Option<Key>> {
    let page = first_page(path)?;
    let mut derived = [0; 32];
    pbkdf2::pbkdf2_hmac::<sha2::Sha512>(master, &page[..16], 256000, &mut derived);
    Ok(Key::checked(&derived, &page))
}
fn strip_account_suffix(account: &str) -> String {
    match account.rsplit_once('_') {
        Some((prefix, suffix))
            if suffix.len() == 4 && suffix.chars().all(|c| c.is_alphanumeric() || c == '_') =>
        {
            prefix.into()
        }
        _ => account.into(),
    }
}

fn discover_accounts(directory: &Path) -> Result<Vec<PathBuf>> {
    let mut roots = vec![directory.to_owned()];
    if let Some(value) = std::env::var_os("WECHAT_LITE_DB_DIR") {
        roots.insert(0, value.into());
    }
    for variable in ["APPDATA", "LOCALAPPDATA"] {
        if let Some(root) = std::env::var_os(variable) {
            for suffix in [
                "Tencent/xwechat",
                "Tencent/xwechat/config",
                "Tencent/WeChat",
            ] {
                let folder = PathBuf::from(&root).join(suffix);
                let Ok(entries) = std::fs::read_dir(folder) else {
                    continue;
                };
                for entry in entries.flatten() {
                    if entry.file_type().is_ok_and(|f| f.is_file()) {
                        let mut bytes = Vec::new();
                        if File::open(entry.path())
                            .and_then(|f| f.take(8192).read_to_end(&mut bytes))
                            .is_ok()
                        {
                            let text = std::str::from_utf8(&bytes)
                                .map(str::to_owned)
                                .unwrap_or_else(|_| encoding_rs::GBK.decode(&bytes).0.into_owned());
                            if let Some(path) = config_path(&text) {
                                roots.push(path);
                            }
                        }
                    }
                }
            }
        }
    }
    #[cfg(windows)]
    roots.extend(windows::registry_paths().into_iter().map(PathBuf::from));
    if let Some(profile) = std::env::var_os("USERPROFILE") {
        roots.push(PathBuf::from(&profile).join("Documents"));
        roots.push(profile.into());
    }
    let mut accounts = Vec::new();
    for root in roots {
        for candidate in [
            root.clone(),
            root.join("xwechat_files"),
            root.join("WeChat Files"),
            root.join("xwechat_files_data"),
        ] {
            if candidate.join("db_storage").is_dir() {
                accounts.push(candidate);
                continue;
            }
            if let Ok(entries) = std::fs::read_dir(candidate) {
                for entry in entries.flatten() {
                    if entry.path().join("db_storage").is_dir() {
                        accounts.push(entry.path());
                    }
                }
            }
        }
    }
    accounts.sort();
    accounts.dedup();
    ensure!(
        !accounts.is_empty(),
        "未发现本机微信数据目录，请检查当前登录账号"
    );
    Ok(accounts)
}
fn config_path(text: &str) -> Option<PathBuf> {
    let text = text.trim().trim_start_matches('\u{feff}');
    if let Ok(value) = serde_json::from_str::<Value>(text) {
        for key in [
            "dataDir",
            "data_dir",
            "fileSavePath",
            "savePath",
            "path",
            "defaultFileSavePath",
        ] {
            if let Some(path) = value[key].as_str().filter(|s| !s.trim().is_empty()) {
                return Some(path.trim().into());
            }
        }
        if let Some(array) = value.as_array() {
            for value in array {
                if let Some(path) = value.as_str().filter(|s| windows_path(s)) {
                    return Some(path.into());
                }
            }
        }
    }
    if windows_path(text) {
        return Some(text.into());
    }
    for (index, _) in text.char_indices() {
        let suffix = &text[index..];
        if windows_path(suffix) {
            let end = suffix
                .find(|c: char| c.is_control() || matches!(c, '"' | '\''))
                .unwrap_or(suffix.len());
            return Some(suffix[..end].trim().into());
        }
    }
    None
}
fn windows_path(text: &str) -> bool {
    let bytes = text.as_bytes();
    bytes.len() >= 3
        && bytes[0].is_ascii_alphabetic()
        && bytes[1] == b':'
        && matches!(bytes[2], b'\\' | b'/')
}

pub fn scan_memory(visitor: impl FnMut(u32, usize, &[u8]) -> bool) -> Result<()> {
    #[cfg(windows)]
    {
        windows::scan_memory(visitor)
    }
    #[cfg(not(windows))]
    {
        let _ = visitor;
        bail!("只读微信进程扫描仅支持 Windows")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::params;

    fn table_schema(group: &str) -> String {
        let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
        format!(
            "CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content BLOB,source TEXT,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);CREATE INDEX {table}_order ON {table}(sort_seq,local_id);"
        )
    }

    fn encrypted_fixture() -> (tempfile::TempDir, Db, Connection, Key) {
        let directory = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(directory.path().join("db_storage/message")).unwrap();
        let connection = Connection::open_in_memory().unwrap();
        connection.execute_batch("PRAGMA page_size=4096;").unwrap();
        let mut reserve = 80i32;
        assert_eq!(
            unsafe {
                rusqlite::ffi::sqlite3_file_control(
                    connection.handle(),
                    c"main".as_ptr(),
                    rusqlite::ffi::SQLITE_FCNTL_RESERVE_BYTES,
                    (&mut reserve as *mut i32).cast(),
                )
            },
            rusqlite::ffi::SQLITE_OK
        );
        connection
            .execute_batch("CREATE TABLE Name2Id(user_name TEXT);")
            .unwrap();
        connection
            .execute_batch(&table_schema("g@chatroom"))
            .unwrap();
        let key = Key {
            data: [7; 32],
            explicit_salt: None,
        };
        let path = directory.path().join("db_storage/message/message_0.db");
        std::fs::write(
            &path,
            crypto::tests::encrypt(&connection.serialize("main").unwrap(), &key),
        )
        .unwrap();
        let mut db = Db::media_fixture(
            directory.path().to_owned(),
            "wxid_fixture",
            None,
            HashMap::new(),
        );
        db.files = collect_files(directory.path()).unwrap();
        db.keys.insert("message/message_0.db".into(), key.clone());
        (directory, db, connection, key)
    }

    fn insert_row(connection: &Connection, group: &str, id: i64, seq: i64) {
        let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
        connection.execute(&format!("INSERT INTO {table} VALUES(?1,1,2,1700000000,'Fixture body','',NULL,NULL,?2,?3)"),params![id,1000+id,seq]).unwrap();
    }

    fn write_wal(db: &Db, connection: &Connection, key: &Key) {
        let path = &db.files["message/message_0.db"];
        std::fs::write(
            crypto::wal_path(path).unwrap(),
            crypto::tests::committed_wal(&connection.serialize("main").unwrap(), key),
        )
        .unwrap();
    }

    #[test]
    fn unchanged_negative_queries_skip_decryption_and_do_not_cross_query_or_group() {
        let (_directory, mut db, connection, key) = encrypted_fixture();
        insert_row(&connection, "g@chatroom", 1, 10);
        std::fs::write(
            &db.files["message/message_0.db"],
            crypto::tests::encrypt(&connection.serialize("main").unwrap(), &key),
        )
        .unwrap();
        let after = json!({"sort_seq":10,"shard":"message__message_0.db","local_id":1});
        assert!(db.page("g@chatroom", &after, 1).unwrap().is_empty());
        assert_eq!(db.connections.len(), 1);
        db.release();
        assert!(db.page("g@chatroom", &after, 1).unwrap().is_empty());
        assert!(
            db.connections.is_empty(),
            "unchanged empty page must not deserialize again"
        );
        assert!(db.page("g@chatroom", &after, 2).unwrap().is_empty());
        assert_eq!(
            db.connections.len(),
            1,
            "different limits have distinct cache keys"
        );
        db.release();
        assert_eq!(
            db.page(
                "g@chatroom",
                &json!({"sort_seq":10,"shard":"message__message_0.db","local_id":0}),
                1
            )
            .unwrap()
            .len(),
            1
        );
        assert_eq!(db.recent("g@chatroom", 1).unwrap().len(), 1);
        assert!(
            db.raw_message("g@chatroom", 2, "message__message_0.db")
                .is_err()
        );
        assert_eq!(
            db.raw_message("g@chatroom", 1, "message__message_0.db")
                .unwrap()["local_id"],
            1
        );
        assert!(db.recent("other@chatroom", 1).unwrap().is_empty());
        db.release();
        assert!(db.recent("other@chatroom", 999).unwrap().is_empty());
        assert!(
            db.connections.is_empty(),
            "a missing table can skip every query for the same group and version"
        );
        assert_eq!(db.recent("g@chatroom", 1).unwrap().len(), 1);
    }

    #[test]
    fn wal_changes_invalidate_empty_pages_and_old_connections_cannot_cache_a_new_version() {
        let (_directory, mut db, connection, key) = encrypted_fixture();
        let cursor = json!({"sort_seq":0,"shard":"","local_id":-1});
        assert!(db.page("g@chatroom", &cursor, 200).unwrap().is_empty());
        let old = db.snapshot_versions["message/message_0.db"].clone();
        let signatures = db.signatures().unwrap();
        insert_row(&connection, "g@chatroom", 1, 10);
        write_wal(&db, &connection, &key);
        let current = crypto::version(&db.files["message/message_0.db"])
            .unwrap()
            .unwrap();
        assert_ne!(old, current);
        assert_ne!(signatures, db.signatures().unwrap());
        assert!(
            db.page("g@chatroom", &cursor, 200).unwrap().is_empty(),
            "current round still owns its original snapshot"
        );
        assert!(
            db.negative_pages
                .iter()
                .all(|entry| entry.version != current),
            "old snapshot must never label its result with new disk metadata"
        );
        db.release();
        assert_eq!(db.page("g@chatroom", &cursor, 200).unwrap().len(), 1);
        assert!(db.page("new@chatroom", &cursor, 200).unwrap().is_empty());
        db.release();
        connection
            .execute_batch(&table_schema("new@chatroom"))
            .unwrap();
        insert_row(&connection, "new@chatroom", 2, 10);
        write_wal(&db, &connection, &key);
        assert_eq!(
            db.page("new@chatroom", &cursor, 200).unwrap().len(),
            1,
            "WAL-created group table must invalidate the no-table cache"
        );
    }

    #[test]
    fn message_signatures_ignore_resources_and_detect_replacements_and_catalog_changes() {
        let (_directory, mut db, connection, key) = encrypted_fixture();
        let cursor = json!({"sort_seq":0,"shard":"","local_id":-1});
        assert!(db.page("g@chatroom", &cursor, 200).unwrap().is_empty());
        db.release();
        let base = db.signatures().unwrap();
        let path = db.files["message/message_0.db"].clone();
        let folder = path.parent().unwrap();
        for name in [
            "message_resource.db",
            "message_resource.db-wal",
            "message_0.db-shm",
            "message_x.db",
            "message_99.db-shm",
        ] {
            std::fs::write(folder.join(name), b"ignored resource fixture").unwrap();
        }
        assert_eq!(base, db.signatures().unwrap());
        let old = crypto::signature(&path).unwrap().unwrap();
        let modified = std::fs::metadata(&path).unwrap().modified().unwrap();
        insert_row(&connection, "g@chatroom", 1, 10);
        let replacement = folder.join("replacement.tmp");
        std::fs::write(
            &replacement,
            crypto::tests::encrypt(&connection.serialize("main").unwrap(), &key),
        )
        .unwrap();
        File::options()
            .write(true)
            .open(&replacement)
            .unwrap()
            .set_times(std::fs::FileTimes::new().set_modified(modified))
            .unwrap();
        std::fs::rename(&replacement, &path).unwrap();
        let replaced = crypto::signature(&path).unwrap().unwrap();
        assert_eq!((old.0, old.1), (replaced.0, replaced.1));
        assert_ne!(
            old.2, replaced.2,
            "replacement identity must invalidate same-size, same-time cache entries"
        );
        assert_ne!(base, db.signatures().unwrap());
        assert_eq!(db.page("g@chatroom", &cursor, 200).unwrap().len(), 1);
        db.release();
        let second = folder.join("message_1.db");
        std::fs::copy(&path, &second).unwrap();
        assert_eq!(db.signatures().unwrap().len(), 2);
        db.keys.insert("message/message_1.db".into(), key);
        let rows = db.page("g@chatroom", &cursor, 200).unwrap();
        assert_eq!(rows.len(), 2);
        assert_ne!(
            rows[0]["shard"], rows[1]["shard"],
            "equal sort sequences keep both shard identities"
        );
        db.release();
        std::fs::remove_file(&second).unwrap();
        assert_eq!(db.signatures().unwrap().len(), 1);
        assert!(!db.files.contains_key("message/message_1.db"));
        assert!(!db.keys.contains_key("message/message_1.db"));
        assert!(
            db.negative_pages
                .iter()
                .all(|entry| entry.rel != "message/message_1.db")
        );
        assert_eq!(db.page("g@chatroom", &cursor, 200).unwrap().len(), 1);
    }

    #[test]
    fn negative_metadata_is_bounded_and_in_memory_fixtures_never_use_file_cache() {
        let mut memory = fixture();
        let cursor = json!({"sort_seq":100,"shard":"","local_id":-1});
        assert!(memory.page("g@chatroom", &cursor, 200).unwrap().is_empty());
        assert!(memory.negative_pages.is_empty());
        insert_row(
            memory.connections.get("message/message_0.db").unwrap(),
            "g@chatroom",
            3,
            101,
        );
        assert_eq!(memory.page("g@chatroom", &cursor, 200).unwrap().len(), 1);
        let (_directory, mut db, _connection, _key) = encrypted_fixture();
        for index in 0..NEGATIVE_LIMIT + 4 {
            assert!(
                db.page(&format!("absent-{index}@chatroom"), &cursor, 200)
                    .unwrap()
                    .is_empty()
            );
            db.release();
            assert!(db.negative_pages.len() <= NEGATIVE_LIMIT);
        }
        assert_eq!(db.negative_pages.len(), NEGATIVE_LIMIT);
        assert!(db.connections.is_empty());
        assert!(db.snapshot_versions.is_empty());
    }
    fn fixture() -> Db {
        let contact = Connection::open_in_memory().unwrap();
        contact.execute_batch("CREATE TABLE contact(id INTEGER PRIMARY KEY,username TEXT,nick_name TEXT,remark TEXT,is_in_chat_room INTEGER DEFAULT 1,delete_flag INTEGER DEFAULT 0);CREATE TABLE chat_room(id INTEGER PRIMARY KEY,username TEXT,owner TEXT,ext_buffer BLOB);CREATE TABLE chatroom_member(room_id INTEGER,member_id INTEGER);INSERT INTO contact(id,username,nick_name,remark) VALUES(1,'g@chatroom','Fixture group','Group remark'),(2,'alpha','Alpha nick','Personal remark'),(3,'beta','Beta nick',''),(4,'left@chatroom','Left',''),(5,'deleted@chatroom','Deleted',''),(6,'roomless@chatroom','No room metadata','');UPDATE contact SET is_in_chat_room=0 WHERE id=4;UPDATE contact SET delete_flag=1 WHERE id=5;INSERT INTO chat_room VALUES(1,'g@chatroom','alpha',NULL);INSERT INTO chatroom_member VALUES(1,2),(1,3),(1,999);").unwrap();
        let fields = |id: &str, name: &str| {
            let mut value = vec![10, id.len() as u8];
            value.extend(id.as_bytes());
            value.extend([18, name.len() as u8]);
            value.extend(name.as_bytes());
            let mut wrapped = vec![10, value.len() as u8];
            wrapped.extend(value);
            wrapped
        };
        let mut names = fields("alpha", "Group alpha");
        names.extend(fields("beta", "Group beta"));
        contact
            .execute("UPDATE chat_room SET ext_buffer=?1", [names])
            .unwrap();
        let mut connections = HashMap::from([("contact/contact.db".into(), contact)]);
        let table = format!("Msg_{:x}", Md5::digest(b"g@chatroom"));
        for (index, sender) in [(0, "alpha"), (1, "beta")] {
            let c = Connection::open_in_memory().unwrap();
            c.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);INSERT INTO Name2Id(rowid,user_name) VALUES(7,'{sender}');CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content BLOB,source TEXT,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);CREATE INDEX message_order ON {table}(sort_seq,local_id);")).unwrap();
            for local in 1..=2 {
                let compressed =
                    zstd::stream::encode_all("Compressed fixture 🐾".as_bytes(), 1).unwrap();
                c.execute(&format!("INSERT INTO {table} VALUES(?1,1,?2,1700000000,?3,'<msgsource/>',?4,?5,?6,?7)"),params![local,if index==1 && local==2 {2}else {7},if index==0 && local==2 {""}else {sender},[0u8,255,3],compressed,1000+index*100+local,if index==1 && local==2 {11}else{10}]).unwrap();
            }
            connections.insert(format!("message/message_{index}.db"), c);
        }
        let heads = Connection::open_in_memory().unwrap();
        heads.execute_batch("CREATE TABLE head_image(username TEXT,image_buffer BLOB);INSERT INTO head_image VALUES('alpha',x'010203');").unwrap();
        connections.insert("head_image/head_image.db".into(), heads);
        Db::media_fixture(
            PathBuf::from("fixture-only"),
            "wxid_fixture",
            Some(17),
            connections,
        )
    }
    #[test]
    fn backward_history_cursor_pages_across_equal_sequences_and_shards_without_gaps() {
        let mut db = fixture();
        let mut cursor = None;
        let mut found = Vec::new();
        loop {
            let page = db.older("g@chatroom", cursor.as_ref(), 1).unwrap();
            if page.is_empty() {
                break;
            }
            cursor = Some(page[0].clone());
            found.extend(page);
        }
        assert_eq!(
            found.iter().map(cursor_tuple).collect::<Vec<_>>(),
            vec![
                (11, "message__message_1.db".into(), 2),
                (10, "message__message_1.db".into(), 1),
                (10, "message__message_0.db".into(), 2),
                (10, "message__message_0.db".into(), 1),
            ]
        );
        assert_eq!(
            db.older("g@chatroom", None, 2)
                .unwrap()
                .iter()
                .map(cursor_tuple)
                .collect::<Vec<_>>(),
            found[..2].iter().map(cursor_tuple).collect::<Vec<_>>()
        );
        assert!(db.older("other@chatroom", None, 60).unwrap().is_empty());
        assert!(db.older("g@chatroom", None, 62).is_err());
    }

    #[test]
    fn shard_cursor_sender_and_raw_media_keep_a_unique_identity() {
        let mut db = fixture();
        let mut position = json!({"sort_seq":0,"shard":"","local_id":0});
        let mut all = Vec::new();
        loop {
            let page = db.page("g@chatroom", &position, 1).unwrap();
            if page.is_empty() {
                break;
            }
            position = page[0].clone();
            all.extend(page);
        }
        assert_eq!(all.len(), 4);
        assert_eq!(
            all.iter().map(cursor_tuple).collect::<Vec<_>>(),
            vec![
                (10, "message__message_0.db".into(), 1),
                (10, "message__message_0.db".into(), 2),
                (10, "message__message_1.db".into(), 1),
                (11, "message__message_1.db".into(), 2)
            ]
        );
        assert_eq!(all[0]["sender_username"], "alpha");
        assert_eq!(all[2]["sender_username"], "beta");
        assert_eq!(all[1]["content"], "Compressed fixture 🐾");
        assert_eq!(db.reload("g@chatroom", &all[2]).unwrap(), all[2]);
        let mut wrong = all[2].clone();
        wrong["sort_seq"] = 999.into();
        assert!(db.reload("g@chatroom", &wrong).is_err());
        let raw = db
            .raw_message("g@chatroom", 1, "message__message_1.db")
            .unwrap();
        assert_eq!(raw["server_id"], 1101);
        assert_eq!(raw["packed_info"], json!([0, 255, 3]));
        assert_eq!(raw["sender_username"], "beta");
        assert!(db.raw_message("g@chatroom", 1, "").is_err());
        assert!(
            db.raw_message("other@chatroom", 1, "message__message_1.db")
                .is_err()
        );
        assert_eq!(db.recent("g@chatroom", 2).unwrap()[0]["sort_seq"], 11);
        assert!(db.page("g@chatroom", &position, 0).unwrap().is_empty());
    }
    #[test]
    fn local_groups_members_names_and_avatars_survive_nonmembers_and_bad_optional_data() {
        let mut db = fixture();
        let groups = db.groups().unwrap();
        assert_eq!(groups[0]["member_count"], 2);
        assert_eq!(groups[0]["name"], "Group remark");
        assert_eq!(groups.len(), 2);
        assert_eq!(groups[1]["username"], "roomless@chatroom");
        assert_eq!(groups[1]["member_count"], 0);
        let members = db.members("g@chatroom").unwrap();
        assert_eq!(members.len(), 2);
        assert_eq!(members[0]["is_owner"], true);
        assert_eq!(
            db.display_name("g@chatroom", "alpha").unwrap(),
            "Personal remark"
        );
        assert_eq!(db.display_name("g@chatroom", "beta").unwrap(), "Group beta");
        assert_eq!(db.display_name("direct", "beta").unwrap(), "Beta nick");
        assert_eq!(db.avatar("alpha").unwrap(), Some(vec![1, 2, 3]));
        assert_eq!(db.avatar("unknown").unwrap(), None);
        assert!(room_names(&[0xff]).is_err());
        db.release();
        assert!(db.connections.is_empty());
        assert!(db.names.is_empty());
    }
    #[test]
    fn config_and_account_paths_preserve_drive_root_and_custom_accounts() {
        assert_eq!(
            config_path(r#"{"path":"D:\\"}"#),
            Some(PathBuf::from("D:\\"))
        );
        assert_eq!(
            config_path("\u{feff}E:\\中文 路径\\xwechat_files\n"),
            Some(PathBuf::from("E:\\中文 路径\\xwechat_files"))
        );
        assert_eq!(strip_account_suffix("wxid_example_ab12"), "wxid_example");
        assert_eq!(strip_account_suffix("custom-name"), "custom-name");
        let temp = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(temp.path().join("xwechat_files/custom-user/db_storage")).unwrap();
        assert!(
            discover_accounts(temp.path())
                .unwrap()
                .iter()
                .any(|p| p.ends_with("custom-user"))
        );
    }
    #[test]
    #[ignore = "Read-only live WeChat probe; never injects, starts WeChat, or sends messages"]
    fn read_only_live_probe() {
        let started = Instant::now();
        let temp = tempfile::tempdir().unwrap();
        let mut db = Db::connect(temp.path()).unwrap();
        let groups = db.groups().unwrap();
        let count = groups.len();
        let recent = if let Some(group) = groups.first().and_then(|g| g["username"].as_str()) {
            db.recent(group, 2).unwrap()
        } else {
            Vec::new()
        };
        if let Some(row) = recent.first() {
            let group = groups[0]["username"].as_str().unwrap();
            assert_eq!(db.reload(group, row).unwrap(), *row);
            let raw = db
                .raw_message(
                    group,
                    row["local_id"].as_i64().unwrap(),
                    row["shard"].as_str().unwrap(),
                )
                .unwrap();
            assert!(raw.get("server_id").is_some());
        }
        let changes = db.signatures().unwrap().len();
        db.release();
        println!(
            "live_read_only_ok groups={count} recent_rows={} database_files={} watched_files={changes} cfg_dword_available={} elapsed_ms={}",
            recent.len(),
            db.rels().len(),
            db.cfg_dword.is_some(),
            started.elapsed().as_millis()
        );
    }
}
