//! On-demand local media retrieval, verified staging and native codec boundaries.
#[path = "media/picture.rs"]
mod picture;
#[path = "media/silk.rs"]
mod silk;
#[path = "media/wxgf.rs"]
mod wxgf;

use crate::{db::Db, native::NativeClient};
use anyhow::{Context, Result, bail, ensure};
use md5::Md5;
pub use picture::avatar_data;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fmt,
    fs::{self, File},
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use walkdir::WalkDir;

pub const MAX_FILE: u64 = 512 * 1024 * 1024;
const MAX_IMAGE: u64 = 10 * 1024 * 1024;

#[derive(Debug)]
pub struct MediaPending(pub String);
impl fmt::Display for MediaPending {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for MediaPending {}
fn pending(message: &str) -> anyhow::Error {
    MediaPending(message.to_owned()).into()
}

pub struct PreparedMedia {
    pub path: PathBuf,
    pub kind: String,
    pub md5: String,
    pub size: u64,
    pub name: String,
    pub gif_attachment: bool,
    pub video_attachment: bool,
}

pub struct Media {
    directory: PathBuf,
    cache: PathBuf,
    image_key: Option<[u8; 16]>,
    xor_key: Option<u8>,
    last_key_scan: Option<Instant>,
    downloads: NativeClient,
    download_groups: BTreeSet<String>,
    download_connected: bool,
    #[cfg(test)]
    video_scan_entries: std::cell::Cell<usize>,
}
impl Media {
    /// `directory` is the relay's `data/adapter`, preserving existing cache paths.
    pub fn new(directory: PathBuf) -> Self {
        let directory = std::path::absolute(&directory).unwrap_or(directory);
        let cache = directory.join("wechat/media");
        Self {
            directory: directory.clone(),
            cache,
            image_key: None,
            xor_key: None,
            last_key_scan: None,
            downloads: NativeClient::with_binary(
                directory.join("native-downloads"),
                &include_bytes!("../vendor/WeChatMedia.dll")[..],
                include_str!("../vendor/WeChatMedia.sha256").trim(),
                json!({"capability":"media_download"}),
            ),
            download_groups: BTreeSet::new(),
            download_connected: false,
            #[cfg(test)]
            video_scan_entries: std::cell::Cell::new(0),
        }
    }
    pub fn select(&mut self, groups: &BTreeSet<String>) -> Result<()> {
        self.downloads
            .restrict(&groups.iter().cloned().collect::<Vec<_>>())?;
        self.download_groups = groups.clone();
        self.download_connected = false;
        Ok(())
    }
    fn cache_dir(&self, key: &str) -> Result<PathBuf> {
        owned_dir(&self.cache, &self.cache.join(sha256(key.as_bytes())))
    }
    pub fn inbound(&mut self, db: &mut Db, group: &str, row: &Value, kind: &str) -> Result<Value> {
        let local_id = number(&row["local_id"]);
        let shard = row["shard"].as_str().unwrap_or("");
        let mut raw = row.clone();
        if local_id > 0 && (!raw["packed_info"].is_array() || kind == "voice") {
            if let Ok(extra) = db.raw_message(group, local_id, shard) {
                if let (Some(target), Some(source)) = (raw.as_object_mut(), extra.as_object()) {
                    for (key, value) in source {
                        if target.get(key).is_none_or(|current| current.is_null()) {
                            target.insert(key.clone(), value.clone());
                        }
                    }
                }
            }
        }
        let key = message_key(group, &raw);
        let source = match kind {
            "image" => self.image(db, group, &raw, &key)?,
            "voice" => self.voice(db, group, &raw, &key)?,
            "video" => self.video(db, group, &raw, &key)?,
            "gif" => self.emoji(db, &content(&raw))?,
            "file" => self.file(db, group, &content(&raw), &key)?,
            _ => bail!("媒体类型暂不支持，原消息已保留"),
        };
        let size = fs::metadata(&source)?.len();
        ensure!(size > 0 && size <= MAX_FILE, "媒体为空或超过 512 MiB");
        let mut actual = kind;
        if kind == "gif" {
            let mut header = [0; 6];
            File::open(&source)?.read_exact(&mut header)?;
            if &header != b"GIF87a" && &header != b"GIF89a" {
                actual = "image";
            }
        }
        Ok(
            json!({"id":"","kind":actual,"name":source.file_name().unwrap_or_default().to_string_lossy(),
            "size":size,"sha256":hash_file::<Sha256>(&source)?,"path":source.canonicalize()?.to_string_lossy()}),
        )
    }
    pub fn record_attachment(
        &mut self,
        group: &str,
        key: &str,
        item: &crate::records::Attachment,
    ) -> Result<Value> {
        let resource = DownloadResource {
            kind: if item.kind == "voice" {
                "file"
            } else {
                item.kind
            },
            file_id: item.file_id.clone(),
            aes: item.aes.clone(),
            length: item.length,
            md5: item.md5.clone(),
        };
        resource.validate()?;
        let folder = self.cache_dir(key)?;
        let name: String = item
            .name
            .rsplit(['/', '\\'])
            .next()
            .unwrap_or("附件")
            .chars()
            .filter(|c| !c.is_control())
            .map(|c| if "<>:\"/\\|?*".contains(c) { '_' } else { c })
            .take(150)
            .collect();
        validate_name(&name)?;
        let receipt = folder.join("record-receipt.json");
        if let Ok(bytes) = read_limited(&receipt, 4096) {
            if let Ok(saved) = serde_json::from_slice::<Value>(&bytes) {
                if let Some(name) = saved["name"]
                    .as_str()
                    .filter(|name| validate_name(name).is_ok())
                {
                    let path = folder.join(name);
                    if cached_video_receipt(
                        &path,
                        &receipt,
                        group,
                        key,
                        std::slice::from_ref(&resource),
                    ) {
                        return Ok(
                            json!({"id":"","name":name,"kind":saved["kind"],"size":path.metadata()?.len(),"sha256":hash_file::<Sha256>(&path)?,"path":path.canonicalize()?.to_string_lossy()}),
                        );
                    }
                }
            }
        }
        let source = self.download(group, key, &resource, item.kind == "image")?;
        let size = source.metadata()?.len();
        let digest = hash_file::<Md5>(&source)?;
        let mut destination = folder.join(&name);
        let actual;
        if item.kind == "image" {
            let data = read_limited(&source, MAX_FILE)?;
            let data = if data.starts_with(b"wxgf") {
                wxgf::decode(&data, &folder)?
            } else {
                data
            };
            let (format, _, _) = picture::validate(&data, 12000)?;
            destination = folder.join(format!("image.{}", picture::extension(format)));
            atomic_write(&destination, &data)?;
            actual = if destination
                .extension()
                .is_some_and(|ext| ext.eq_ignore_ascii_case("gif"))
            {
                "gif"
            } else {
                "image"
            };
        } else {
            copy_verified(&source, &destination, Some(size), &[digest.clone()])?;
            if item.kind == "voice" {
                destination = silk::decode(&destination)?;
            }
            actual = item.kind;
        }
        atomic_write(
            &receipt,
            &serde_json::to_vec(
                &json!({"request_id":download_id(group,key,&resource),"size":destination.metadata()?.len(),"md5":hash_file::<Md5>(&destination)?,"name":destination.file_name().unwrap_or_default().to_string_lossy(),"kind":actual}),
            )?,
        )?;
        Ok(
            json!({"id":"","name":destination.file_name().unwrap_or_default().to_string_lossy(),"kind":actual,
            "size":destination.metadata()?.len(),"sha256":hash_file::<Sha256>(&destination)?,"path":destination.canonicalize()?.to_string_lossy()}),
        )
    }
    pub fn outgoing(
        &self,
        data_root: &Path,
        message_id: &str,
        index: usize,
        media: &Value,
    ) -> Result<PreparedMedia> {
        let ident = uuid::Uuid::parse_str(message_id)
            .context("发送编号无效")?
            .to_string();
        let name = media["name"].as_str().context("附件缺少文件名")?;
        validate_name(name)?;
        ensure!(index < 12, "附件数量超过 12 个");
        let media_root = data_root
            .join("media")
            .canonicalize()
            .context("服务端媒体目录不存在")?;
        let original = PathBuf::from(media["path"].as_str().context("附件缺少路径")?);
        let original = original.canonicalize().context("附件尚未在本机完整下载")?;
        ensure!(
            original.starts_with(&media_root) && original.is_file(),
            "附件不在当前服务端的媒体目录"
        );
        let size = fs::metadata(&original)?.len();
        ensure!(size > 0 && size <= MAX_FILE, "附件为空或超过 512 MiB");
        let expected = media["sha256"]
            .as_str()
            .context("附件缺少 SHA256 校验信息")?;
        ensure!(
            is_hex(expected, 64) && number(&media["size"]) == size as i64,
            "附件大小或校验信息无效"
        );
        let root = self.directory.join("feishu/media");
        let folder = owned_dir(&root, &root.join(ident).join(index.to_string()))?;
        let path = folder.join(name);
        ensure!(!path.is_symlink(), "附件暂存文件不能是链接");
        if !path.is_file() {
            let temp = path.with_extension(format!(
                "{}.part",
                path.extension().unwrap_or_default().to_string_lossy()
            ));
            ensure!(!temp.is_symlink(), "附件临时文件不能是链接");
            fs::copy(&original, &temp)?;
            if fs::metadata(&temp)?.len() != size || hash_file::<Sha256>(&temp)? != expected {
                let _ = fs::remove_file(&temp);
                bail!("附件内容校验失败");
            }
            commit(&temp, &path)?;
        }
        ensure!(
            fs::metadata(&path)?.len() == size && hash_file::<Sha256>(&path)? == expected,
            "附件内容校验失败，未提交微信"
        );
        let requested = media["kind"].as_str().unwrap_or("file");
        let mut kind = if matches!(requested, "image" | "gif") {
            "image"
        } else {
            "file"
        };
        if kind == "image" {
            let (format, width, height) =
                picture::read_and_validate(&path, 12000).context("待发送图片无法完整解码")?;
            if format == image::ImageFormat::Gif {
                kind = if size <= 1024 * 1024 && width <= 2000 && height <= 2000 {
                    "gif"
                } else {
                    "file"
                };
            }
        }
        let mut head = [0u8; 12];
        let n = File::open(&path)?.read(&mut head)?;
        Ok(PreparedMedia {
            md5: hash_file::<Md5>(&path)?,
            size,
            name: name.to_owned(),
            gif_attachment: kind == "file" && n >= 6 && matches!(&head[..6], b"GIF87a" | b"GIF89a"),
            video_attachment: kind == "file"
                && path
                    .extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case("mp4"))
                && n >= 8
                && &head[4..8] == b"ftyp",
            path: path.canonicalize()?,
            kind: kind.to_owned(),
        })
    }
    fn image(&mut self, db: &mut Db, group: &str, row: &Value, key: &str) -> Result<PathBuf> {
        match self.local_image(db, group, row, key) {
            Ok(path) => Ok(path),
            Err(local_error) => {
                let xml = content(row);
                let Some(resource) = image_resource(&xml) else {
                    return Err(local_error);
                };
                let source = self.download(group, key, &resource, false)?;
                let folder = self.cache_dir(key)?;
                // The CDN API writes decrypted original bytes; no foreground image-key scan is needed.
                let data = read_limited(&source, MAX_FILE)?;
                let plain = if data.starts_with(b"wxgf") {
                    wxgf::decode(&data, &folder)?
                } else {
                    data
                };
                let (format, _, _) = picture::validate(&plain, 12000)?;
                let destination = folder.join(format!("image.{}", picture::extension(format)));
                atomic_write(&destination, &plain)?;
                Ok(destination)
            }
        }
    }
    fn local_image(&mut self, db: &mut Db, group: &str, row: &Value, key: &str) -> Result<PathBuf> {
        let folder = self.cache_dir(key)?;
        if let Some(path) = cached_picture(&folder) {
            return Ok(path);
        }
        let digest = find_md5(row).ok_or_else(|| pending("微信图片元数据尚未就绪，会自动重试"))?;
        let root = db
            .account_dir
            .join("msg/attach")
            .join(picture::md5(group.as_bytes()));
        let mut mid = None;
        let mut original = None;
        for entry in WalkDir::new(&root)
            .follow_links(false)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            if !entry.file_type().is_file() {
                continue;
            }
            if entry.file_name() == format!("{digest}_h.dat").as_str()
                && entry.metadata().is_ok_and(|m| m.len() >= 1024)
            {
                original = Some(entry.into_path());
            } else if entry.file_name() == format!("{digest}.dat").as_str() {
                mid = Some(entry.into_path());
            }
        }
        let source = original
            .or(mid)
            .ok_or_else(|| pending("微信图片尚未完整下载，只有缩略图时不会转发"))?;
        let source = within(&root, &source)?;
        let data = read_limited(&source, MAX_FILE)
            .map_err(|_| pending("微信图片缓存正在写入，会自动重试"))?;
        let aes = if data.starts_with(picture::V2) {
            ensure!(data.len() >= 31, "微信图片缓存尚未完整写入");
            Some(self.resolve_image_key(db, &data[15..31])?)
        } else {
            None
        };
        let xor = self.resolve_xor_key(db, &source, &data);
        let plain = picture::decrypt(&data, aes.as_ref(), xor)
            .map_err(|_| pending("微信图片缓存尚未完整解密，会自动重试；可在微信打开大图后等待"))?;
        let plain = if plain.starts_with(b"wxgf") {
            wxgf::decode(&plain, &folder)?
        } else {
            plain
        };
        let (format, _, _) = picture::validate(&plain, 12000)
            .map_err(|_| pending("微信图片缓存尚未完整解码，会自动重试"))?;
        let dest = folder.join(format!("image.{}", picture::extension(format)));
        atomic_write(&dest, &plain)?;
        Ok(dest)
    }
    fn resolve_image_key(&mut self, db: &Db, probe: &[u8]) -> Result<[u8; 16]> {
        if let Some(key) = self
            .image_key
            .filter(|key| picture::verifies_key(key, probe))
        {
            return Ok(key);
        }
        if let Some(cfg) = db.cfg_dword {
            let (key, _) = picture::image_keys(cfg, &db.wxid);
            if picture::verifies_key(&key, probe) {
                self.image_key = Some(key);
                return Ok(key);
            }
        }
        let store = self.directory.join("wechat/db/image_keys.json");
        if let Ok(data) = fs::read(&store) {
            if data.len() <= 1024 * 1024 {
                if let Ok(saved) = serde_json::from_slice::<Value>(&data) {
                    if let Some(text) = saved[&db.account].as_str() {
                        if let Ok(key) = <[u8; 16]>::try_from(text.as_bytes()) {
                            if picture::verifies_key(&key, probe) {
                                self.image_key = Some(key);
                                return Ok(key);
                            }
                        }
                    }
                }
            }
        }
        if self
            .last_key_scan
            .is_some_and(|last| last.elapsed() < Duration::from_secs(30))
        {
            return Err(pending(
                "微信图片解密密钥未就绪，请在微信点击任意图片查看大图，随后自动重试",
            ));
        }
        self.last_key_scan = Some(Instant::now());
        let mut found = None;
        crate::db::scan_memory(|_, _, data| {
            found = scan_image_key(data, probe);
            found.is_some()
        })?;
        if let Some(key) = found {
            self.image_key = Some(key);
            let mut saved = fs::read(&store)
                .ok()
                .filter(|data| data.len() < 1024 * 1024)
                .and_then(|data| serde_json::from_slice::<Value>(&data).ok())
                .filter(Value::is_object)
                .unwrap_or_else(|| json!({}));
            saved[&db.account] = json!(String::from_utf8_lossy(&key));
            if let Some(folder) = store.parent() {
                fs::create_dir_all(folder)?;
            }
            atomic_write(&store, &serde_json::to_vec(&saved)?)?;
            return Ok(key);
        }
        Err(pending(
            "微信图片解密密钥未加载，请在微信点击任意图片查看大图后等待重试",
        ))
    }
    fn resolve_xor_key(&mut self, db: &Db, source: &Path, data: &[u8]) -> u8 {
        if data.starts_with(picture::V1) {
            return xor_from_tail(source);
        }
        if let Some(cfg) = db.cfg_dword.filter(|cfg| *cfg != 0) {
            let (key, xor) = picture::image_keys(cfg, &db.wxid);
            if !data.starts_with(picture::V2)
                || data
                    .get(15..31)
                    .is_some_and(|probe| picture::verifies_key(&key, probe))
            {
                self.xor_key = Some(xor);
                return xor;
            }
        }
        if let Some(key) = self.xor_key {
            return key;
        }
        let root = db.account_dir.join("msg/attach");
        let mut templates = Vec::new();
        for entry in WalkDir::new(&root)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
        {
            if !entry.file_type().is_file()
                || !entry.file_name().to_string_lossy().ends_with("_t.dat")
            {
                continue;
            }
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            let Ok(modified) = meta.modified() else {
                continue;
            };
            templates.push((modified, entry.into_path()));
            templates.sort_by(|a, b| b.0.cmp(&a.0));
            templates.truncate(16);
        }
        let mut votes = std::collections::BTreeMap::<u8, usize>::new();
        for (_, path) in templates {
            let Ok(path) = within(&root, &path) else {
                continue;
            };
            let Ok(mut file) = File::open(path) else {
                continue;
            };
            let mut tail = [0; 2];
            if file.seek(SeekFrom::End(-2)).is_err() || file.read_exact(&mut tail).is_err() {
                continue;
            }
            let key = tail[0] ^ 0xff;
            if tail[1] ^ key == 0xd9 {
                *votes.entry(key).or_default() += 1;
            }
        }
        let key = votes
            .into_iter()
            .max_by_key(|(_, count)| *count)
            .map(|(key, _)| key)
            .unwrap_or(0x88);
        self.xor_key = Some(key);
        key
    }
    fn voice(&self, db: &mut Db, group: &str, row: &Value, key: &str) -> Result<PathBuf> {
        let directory = self.cache_dir(key)?;
        let silk = directory.join("voice.silk");
        let server = number(&row["server_id"]);
        ensure!(server != 0, "微信语音尚无服务端编号，源消息已保留");
        if silk.is_file() {
            if let Ok(wav) = silk::decode(&silk) {
                return Ok(wav);
            }
        }
        {
            let mut voice = None;
            for rel in db
                .rels()
                .into_iter()
                .filter(|rel| rel.rsplit(['/', '\\', '_']).any(|part| part == "media"))
            {
                let Ok(conn) = db.open(&rel) else {
                    continue;
                };
                let query = "SELECT v.voice_data FROM VoiceInfo v JOIN Name2Id n ON n.rowid=v.chat_name_id WHERE n.user_name=?1 AND v.svr_id=?2 AND length(v.voice_data)>0 AND length(v.voice_data)<=?3 ORDER BY v.create_time DESC LIMIT 1";
                if let Ok(bytes) = conn.query_row(
                    query,
                    rusqlite::params![group, server, MAX_FILE as i64],
                    |r| r.get::<_, Vec<u8>>(0),
                ) {
                    voice = Some(bytes);
                    break;
                }
            }
            let voice = voice.ok_or_else(|| pending("微信语音尚未完整写入本机缓存，会自动补发"))?;
            atomic_write(&silk, &voice)?;
        }
        silk::decode(&silk).map_err(|_| pending("微信语音缓存尚未完整解码，原件已保留，会自动重试"))
    }
    fn video(&mut self, db: &Db, group: &str, row: &Value, key: &str) -> Result<PathBuf> {
        let xml = content(row);
        let doc = xml_document(&xml)?;
        let video = doc
            .descendants()
            .find(|node| node.has_tag_name("videomsg"))
            .ok_or_else(|| pending("微信视频元数据尚未就绪"))?;
        let standard_length = video
            .attribute("length")
            .and_then(|v| v.parse::<u64>().ok())
            .filter(|v| *v > 0)
            .ok_or_else(|| pending("微信视频大小尚未就绪"))?;
        let standard_hashes: Vec<String> = ["md5", "newmd5"]
            .iter()
            .filter_map(|key| video.attribute(*key))
            .filter(|value| is_hex(value, 32))
            .map(str::to_lowercase)
            .collect();
        let resource = video_resource(video)?;
        let length = resource.length;
        let hashes = vec![resource.md5.clone()];
        let mut resources = vec![resource];
        if resources[0].kind == "video_original" {
            if let Ok(standard) = standard_video_resource(video) {
                resources.push(standard);
            }
        }
        let matches_video = |path: &Path| {
            verified(path, length, &hashes)
                || (standard_length <= MAX_FILE
                    && verified(path, standard_length, &standard_hashes))
        };
        let request_key = format!("video:{key}");
        let ident = sha256(request_key.as_bytes());
        let directory = self.cache_dir(&request_key)?;
        let destination = directory.join("video.mp4");
        let receipt = directory.join("video-receipt.json");
        if cached_video_receipt(&destination, &receipt, group, key, &resources) {
            return Ok(destination);
        }
        let native_root = self.directory.join("feishu/media");
        owned_dir(&self.directory, &native_root)?;
        let partial = native_root
            .join("wechat-video")
            .join(format!("{ident}.mp4.part"));
        // Native downloads finish in the owned partial path. Check both known paths before
        // walking WeChat's potentially large video archive on each pending-media retry.
        for (root, path) in [(&self.cache, &destination), (&native_root, &partial)] {
            let Ok(source) = within(root, path) else {
                continue;
            };
            if matches_video(&source) {
                if source != destination {
                    let size = source.metadata()?.len();
                    let allowed = if size == length {
                        hashes.clone()
                    } else {
                        standard_hashes.clone()
                    };
                    copy_verified(&source, &destination, Some(size), &allowed)?;
                }
                return Ok(destination.clone());
            }
        }
        let root = db.account_dir.join("msg/video");
        let id = find_md5(row);
        for entry in WalkDir::new(&root)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
        {
            #[cfg(test)]
            self.video_scan_entries
                .set(self.video_scan_entries.get() + 1);
            if entry.file_type().is_file()
                && entry
                    .metadata()
                    .is_ok_and(|m| m.len() == length || m.len() == standard_length)
                && (id
                    .as_ref()
                    .is_some_and(|id| entry.file_name() == format!("{id}.mp4").as_str())
                    || entry
                        .path()
                        .extension()
                        .is_some_and(|ext| ext.eq_ignore_ascii_case("mp4")))
            {
                if let Ok(source) = within(&root, entry.path())
                    && matches_video(&source)
                {
                    let size = source.metadata()?.len();
                    let allowed = if size == length {
                        hashes.clone()
                    } else {
                        standard_hashes.clone()
                    };
                    copy_verified(&source, &destination, Some(size), &allowed)?;
                    return Ok(destination);
                }
            }
        }
        let mut last_error = pending("微信视频等待后台下载");
        for resource in &resources {
            match self.download(group, key, resource, false) {
                Ok(source) => {
                    let size = source.metadata()?.len();
                    let digest = hash_file::<Md5>(&source)?;
                    copy_verified(&source, &destination, Some(size), &[digest.clone()])?;
                    atomic_write(
                        &receipt,
                        &serde_json::to_vec(&json!({
                            "request_id":download_id(group,key,resource),"size":size,"md5":digest,
                        }))?,
                    )?;
                    return Ok(destination);
                }
                Err(error) => last_error = error,
            }
        }
        Err(last_error)
    }
    fn file(&mut self, db: &Db, group: &str, xml: &str, key: &str) -> Result<PathBuf> {
        let document = xml_document(xml)?;
        let app = document
            .descendants()
            .find(|node| node.has_tag_name("appmsg"))
            .context("微信文件缺少附件信息")?;
        ensure!(
            matches!(text_at(app, "type"), "6" | "74"),
            "微信消息没有有效的文件附件信息"
        );
        let name = text_at(app, "title");
        validate_name(name)?;
        let digest = ["md5", "filemd5"]
            .into_iter()
            .map(|tag| text_at(app, tag))
            .find(|value| !value.is_empty())
            .unwrap_or("");
        if digest.is_empty() {
            return Err(pending("微信文件 MD5 尚未就绪，完整后会自动重试"));
        }
        ensure!(is_hex(digest, 32), "微信文件 MD5 校验信息无效");
        let digest = digest.to_lowercase();
        let length = text_at(app, "totallen");
        let expected = if length.is_empty() {
            None
        } else {
            Some(
                length
                    .parse::<u64>()
                    .ok()
                    .filter(|v| *v > 0)
                    .context("微信文件大小信息无效")?,
            )
        };
        ensure!(
            expected.is_none_or(|length| length <= MAX_FILE),
            "文件超过 512 MiB"
        );
        let directory = self.cache_dir(key)?;
        let destination = directory.join(name);
        let matches = |path: &Path| -> bool {
            let Ok(size) = fs::metadata(path).map(|m| m.len()) else {
                return false;
            };
            size > 0
                && size <= MAX_FILE
                && expected.is_none_or(|length| size == length)
                && hash_file::<Md5>(path).is_ok_and(|value| value == digest)
        };
        if !destination.is_symlink() && matches(&destination) {
            return Ok(destination);
        }
        let root = db.account_dir.join("msg/file");
        let mut candidates: Vec<_> = WalkDir::new(&root)
            .follow_links(false)
            .into_iter()
            .filter_map(|entry| entry.ok())
            .filter(|entry| {
                entry.file_type().is_file()
                    && entry.metadata().is_ok_and(|m| {
                        m.len() > 0
                            && m.len() <= MAX_FILE
                            && expected
                                .map_or_else(|| entry.file_name() == name, |size| m.len() == size)
                    })
            })
            .map(|entry| entry.into_path())
            .collect();
        candidates.sort_by_key(|path| path.file_name().is_none_or(|filename| filename != name));
        for candidate in candidates {
            let Ok(source) = within(&root, &candidate) else {
                continue;
            };
            if !matches(&source) {
                continue;
            }
            copy_verified(&source, &destination, expected, &[digest.clone()])?;
            return Ok(destination);
        }
        let resource = DownloadResource {
            kind: "file",
            file_id: text_at(app, "cdnattachurl").to_owned(),
            aes: text_at(app, "aeskey").to_owned(),
            length: expected.unwrap_or(0),
            md5: digest.clone(),
        };
        let source = self.download(group, key, &resource, false)?;
        copy_verified(&source, &destination, expected, &[digest])?;
        Ok(destination)
    }
    fn download(&mut self, group: &str, key: &str, resource: &DownloadResource, allow_transcoded_image: bool) -> Result<PathBuf> {
        resource
            .validate()
            .map_err(|error| pending(&error.to_string()))?;
        let native_root = self.directory.join("feishu/media");
        let folder = owned_dir(&self.directory, &native_root.join("wechat-download"))?;
        let id = download_id(group, key, resource);
        let partial = folder.join(format!("{id}.part"));
        ensure!(!partial.is_symlink(), "微信下载缓存不能是链接");
        if let Ok(source) = within(&folder, &partial) {
            if verified(&source, resource.length, &[resource.md5.clone()]) {
                return Ok(source);
            }
        }
        ensure!(
            self.download_groups.contains(group),
            "该群未授权后台媒体下载"
        );
        if !self.download_connected || !self.downloads.is_alive() {
            let health = self
                .downloads
                .connect(
                    &self.download_groups.iter().cloned().collect::<Vec<_>>(),
                    &native_root,
                )
                .map_err(|error| pending(&format!("微信后台下载组件未就绪：{error}")))?;
            ensure!(
                health["capabilities"]
                    .as_array()
                    .is_some_and(|caps| caps.iter().any(|cap| cap == "media_download"))
                    && health["capabilities"]
                        .as_array()
                        .is_some_and(|caps| caps.iter().any(|cap| cap == "download_receipts"))
                    && health["max_size"].as_u64() == Some(MAX_FILE)
                    && health["hooks_installed"] == true,
                "微信后台下载组件能力不匹配"
            );
            self.downloads.request("/start", Some(&json!({})))?;
            self.download_connected = true;
        }
        let response = self.downloads.request("/DownloadMedia", Some(&json!({
            "group":group,"kind":resource.kind,"file_id":resource.file_id,"aes_key":resource.aes,
            "length":resource.length,"request_id":id,
        }))).map_err(|error| pending(&format!("微信后台媒体下载等待重试：{error}")))?;
        if let Some(source) = completed_download(&response, &folder, &partial, &id, resource, allow_transcoded_image)? {
            return Ok(source);
        }
        if response["status"] == "failed" {
            return Err(pending(&format!(
                "微信 CDN 暂未提供此资源（{}），会自动重试",
                response["code"]
            )));
        }
        ensure!(response["status"] == "downloading", "微信后台下载状态无效");
        Err(pending("微信媒体正在后台下载，大小和 MD5 校验后自动转发"))
    }
    fn emoji(&self, db: &Db, xml: &str) -> Result<PathBuf> {
        let document = xml_document(xml)?;
        let emoji = document
            .descendants()
            .find(|node| node.has_tag_name("emoji"))
            .context("微信表情缺少资源信息")?;
        let digest = emoji.attribute("md5").unwrap_or("").to_lowercase();
        ensure!(is_hex(&digest, 32), "微信表情 MD5 无效");
        let length = emoji.attribute("len").unwrap_or("");
        let expected = if length.is_empty() {
            None
        } else {
            Some(
                length
                    .parse::<u64>()
                    .ok()
                    .filter(|v| *v > 0)
                    .context("微信表情大小信息无效")?,
            )
        };
        ensure!(
            expected.is_none_or(|size| size <= MAX_IMAGE),
            "微信表情超过 10 MiB"
        );
        let folder = owned_dir(&self.cache, &self.cache.join("emoji").join(&digest))?;
        let matches = |data: &[u8]| -> bool {
            data.len() > 0
                && data.len() as u64 <= MAX_IMAGE
                && expected.is_none_or(|size| size == data.len() as u64)
                && picture::md5(data) == digest
        };
        for entry in fs::read_dir(&folder)? {
            let entry = entry?;
            let path = entry.path();
            if path.is_symlink()
                || !path.is_file()
                || path.extension().is_some_and(|ext| ext == "part")
            {
                continue;
            }
            if let Ok(data) = read_limited(&path, MAX_IMAGE) {
                if matches(&data)
                    && picture::validate(
                        &data,
                        if data.starts_with(b"GIF8") {
                            2000
                        } else {
                            12000
                        },
                    )
                    .is_ok()
                {
                    return Ok(path);
                }
            }
        }
        // Newer WeChat versions may already keep the verified original in local emoji caches.
        for root in [
            db.account_dir.join("emoticon"),
            db.account_dir.join("msg/emoticon"),
            db.account_dir.join("cache/emoji"),
        ] {
            for entry in WalkDir::new(&root)
                .follow_links(false)
                .into_iter()
                .filter_map(|e| e.ok())
            {
                if !entry.file_type().is_file()
                    || !entry
                        .file_name()
                        .to_string_lossy()
                        .to_lowercase()
                        .contains(&digest)
                {
                    continue;
                }
                let Ok(source) = within(&root, entry.path()) else {
                    continue;
                };
                let Ok(data) = read_limited(&source, MAX_IMAGE) else {
                    continue;
                };
                if matches(&data) {
                    if let Ok((format, _, _)) = picture::validate(
                        &data,
                        if data.starts_with(b"GIF8") {
                            2000
                        } else {
                            12000
                        },
                    ) {
                        let destination =
                            folder.join(format!("{digest}.{}", picture::extension(format)));
                        atomic_write(&destination, &data)?;
                        return Ok(destination);
                    }
                }
            }
        }
        let urls: Vec<_> = ["cdnurl", "encrypturl"]
            .into_iter()
            .filter_map(|key| emoji.attribute(key))
            .filter(|v| !v.is_empty())
            .collect();
        ensure!(!urls.is_empty(), "微信表情没有原资源地址，源消息已保留");
        for url in urls {
            let Ok(mut data) = emoji_resource(url) else {
                continue;
            };
            if !matches(&data) {
                let key = emoji.attribute("aeskey").unwrap_or("");
                if !is_hex(key, 32) {
                    continue;
                }
                let key: [u8; 16] = hex::decode(key)?.try_into().unwrap();
                let Ok(plain) = picture::ecb_unpad(&data, &key) else {
                    continue;
                };
                data = plain;
            }
            if !matches(&data) {
                continue;
            }
            let Ok((format, _, _)) = picture::validate(
                &data,
                if data.starts_with(b"GIF8") {
                    2000
                } else {
                    12000
                },
            ) else {
                continue;
            };
            let destination = folder.join(format!("{digest}.{}", picture::extension(format)));
            atomic_write(&destination, &data)?;
            return Ok(destination);
        }
        Err(pending(
            "微信表情原资源尚未下载或未通过完整校验，会自动重试",
        ))
    }
}

struct DownloadResource {
    kind: &'static str,
    file_id: String,
    aes: String,
    length: u64,
    md5: String,
}
impl DownloadResource {
    fn validate(&self) -> Result<()> {
        ensure!(
            (1..=MAX_FILE).contains(&self.length),
            "媒体大小缺失或超过 512 MiB"
        );
        ensure!(is_hex(&self.md5, 32), "媒体缺少有效的 MD5 校验信息");
        ensure!(
            (32..=4096).contains(&self.file_id.len())
                && self.file_id.len() % 2 == 0
                && is_hex(&self.file_id, self.file_id.len())
                && is_hex(&self.aes, 32),
            "微信媒体缺少有效的后台下载参数，原消息已保留"
        );
        Ok(())
    }
}
fn completed_download(response: &Value, folder: &Path, partial: &Path, id: &str, resource: &DownloadResource, allow_transcoded_image: bool) -> Result<Option<PathBuf>> {
    ensure!(response["ret"] == 0 && response["request_id"] == id, "微信后台下载回执不匹配");
    let returned = PathBuf::from(response["path"].as_str().unwrap_or(""));
    ensure!(absolute_comparable(&returned) == absolute_comparable(partial), "微信后台下载路径不匹配");
    if response["status"] != "complete" { return Ok(None); }
    let source = within(folder, partial)?;
    let size = response["size"].as_u64().unwrap_or(0);
    let digest = response["md5"].as_str().unwrap_or("").to_ascii_lowercase();
    ensure!((1..=MAX_FILE).contains(&size) && is_hex(&digest, 32) && verified(&source,size,&[digest.clone()]), "微信 CDN 下载完成回执的文件校验失败");
    // Full record images can be reencoded by the CDN; only their same-request native receipt can attest replacement bytes.
    ensure!(resource.kind == "video" || (allow_transcoded_image && resource.kind == "image") || (size == resource.length && digest == resource.md5), "微信 CDN 下载内容与原消息不一致");
    Ok(Some(source))
}

fn download_id(group: &str, key: &str, resource: &DownloadResource) -> String {
    sha256(
        json!([
            group,
            key,
            resource.kind,
            resource.file_id,
            resource.aes,
            resource.length,
            resource.md5
        ])
        .to_string()
        .as_bytes(),
    )
}
fn image_resource(xml: &str) -> Option<DownloadResource> {
    let document = xml_document(xml).ok()?;
    let image = document
        .descendants()
        .find(|node| node.has_tag_name("img"))?;
    let resource = DownloadResource {
        kind: "image",
        file_id: image.attribute("cdnbigimgurl").unwrap_or("").to_owned(),
        aes: image.attribute("aeskey").unwrap_or("").to_owned(),
        length: image
            .attribute("hdlength")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0),
        md5: image.attribute("md5").unwrap_or("").to_ascii_lowercase(),
    };
    resource.validate().ok()?;
    Some(resource)
}
fn video_resource(video: roxmltree::Node<'_, '_>) -> Result<DownloadResource> {
    let original = DownloadResource {
        kind: "video_original",
        file_id: video.attribute("cdnrawvideourl").unwrap_or("").to_owned(),
        aes: video
            .attribute("cdnrawvideoaeskey")
            .unwrap_or("")
            .to_owned(),
        length: video
            .attribute("rawlength")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0),
        md5: video.attribute("rawmd5").unwrap_or("").to_ascii_lowercase(),
    };
    if original.validate().is_ok() {
        return Ok(original);
    }
    standard_video_resource(video)
}
fn standard_video_resource(video: roxmltree::Node<'_, '_>) -> Result<DownloadResource> {
    let resource = DownloadResource {
        kind: "video",
        file_id: video.attribute("cdnvideourl").unwrap_or("").to_owned(),
        aes: video.attribute("aeskey").unwrap_or("").to_owned(),
        length: video
            .attribute("length")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0),
        md5: ["md5", "newmd5"]
            .iter()
            .filter_map(|key| video.attribute(*key))
            .find(|v| is_hex(v, 32))
            .unwrap_or("")
            .to_ascii_lowercase(),
    };
    // Cached local videos may have no CDN locator. Validate it only if a download is necessary.
    ensure!(
        (1..=MAX_FILE).contains(&resource.length) && is_hex(&resource.md5, 32),
        "微信视频大小或 MD5 无效"
    );
    Ok(resource)
}
fn cached_video_receipt(
    path: &Path,
    receipt: &Path,
    group: &str,
    key: &str,
    resources: &[DownloadResource],
) -> bool {
    if path.is_symlink() || receipt.is_symlink() {
        return false;
    }
    let Ok(bytes) = read_limited(receipt, 4096) else {
        return false;
    };
    let Ok(record) = serde_json::from_slice::<Value>(&bytes) else {
        return false;
    };
    let size = record["size"].as_u64().unwrap_or(0);
    let digest = record["md5"].as_str().unwrap_or("");
    (1..=MAX_FILE).contains(&size)
        && is_hex(digest, 32)
        && resources
            .iter()
            .any(|resource| record["request_id"] == download_id(group, key, resource))
        && verified(path, size, &[digest.to_owned()])
}

fn content(row: &Value) -> String {
    match &row["content"] {
        Value::String(value) => value.clone(),
        value => String::from_utf8_lossy(&blob(value)).into_owned(),
    }
}
fn blob(value: &Value) -> Vec<u8> {
    value
        .as_array()
        .map(|array| {
            array
                .iter()
                .filter_map(|byte| byte.as_u64().and_then(|n| u8::try_from(n).ok()))
                .collect()
        })
        .unwrap_or_default()
}
fn number(value: &Value) -> i64 {
    value
        .as_i64()
        .or_else(|| value.as_u64().map(|value| value as i64))
        .or_else(|| {
            value
                .as_str()
                .and_then(|v| v.parse::<i128>().ok())
                .map(|v| v as i64)
        })
        .unwrap_or(0)
}
fn message_key(group: &str, row: &Value) -> String {
    format!(
        "wx:{group}:{}:{}:{}",
        row["shard"].as_str().unwrap_or(""),
        number(&row["local_id"]),
        number(&row["sort_seq"])
    )
}
fn find_md5(row: &Value) -> Option<String> {
    for value in [&row["packed_info"], &row["content"]] {
        let data = value
            .as_str()
            .map(|v| v.as_bytes().to_vec())
            .unwrap_or_else(|| blob(value));
        for word in data.split(|byte| !byte.is_ascii_hexdigit()) {
            if word.len() >= 32 {
                return Some(String::from_utf8_lossy(&word[..32]).to_ascii_lowercase());
            }
        }
    }
    None
}
fn is_hex(value: &str, length: usize) -> bool {
    value.len() == length && value.as_bytes().iter().all(u8::is_ascii_hexdigit)
}
fn sha256(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}
fn hash_file<D: Digest + Default>(path: &Path) -> Result<String> {
    let mut digest = D::new();
    let mut file = File::open(path)?;
    let mut chunk = [0; 65536];
    loop {
        let n = file.read(&mut chunk)?;
        if n == 0 {
            break;
        }
        digest.update(&chunk[..n]);
    }
    Ok(hex::encode(digest.finalize()))
}
fn read_limited(path: &Path, max: u64) -> Result<Vec<u8>> {
    let size = fs::metadata(path)?.len();
    ensure!(size > 0 && size <= max, "媒体大小无效或超过限制");
    let mut data = Vec::with_capacity(size as usize);
    File::open(path)?.take(max + 1).read_to_end(&mut data)?;
    ensure!(data.len() as u64 == size, "媒体缓存正在写入");
    Ok(data)
}
fn within(root: &Path, path: &Path) -> Result<PathBuf> {
    ensure!(!path.is_symlink(), "媒体缓存不能是链接");
    let root = root.canonicalize()?;
    let path = path.canonicalize()?;
    ensure!(path.starts_with(root), "媒体缓存超出私有目录");
    Ok(path)
}
fn owned_dir(root: &Path, path: &Path) -> Result<PathBuf> {
    for ancestor in path.ancestors() {
        ensure!(!ancestor.is_symlink(), "媒体目录不能是链接");
    }
    fs::create_dir_all(root)?;
    fs::create_dir_all(path)?;
    within(root, path)
}
fn absolute_comparable(path: &Path) -> String {
    path.to_string_lossy()
        .trim_start_matches("\\\\?\\")
        .replace('/', "\\")
        .to_lowercase()
}
fn commit(temp: &Path, destination: &Path) -> Result<()> {
    ensure!(
        !temp.is_symlink() && !destination.is_symlink(),
        "媒体缓存不能是链接"
    );
    #[cfg(windows)]
    {
        if destination.exists() {
            fs::remove_file(destination)?;
        }
    }
    fs::rename(temp, destination)?;
    Ok(())
}
fn atomic_write(path: &Path, data: &[u8]) -> Result<()> {
    ensure!(!path.is_symlink(), "媒体缓存不能是链接");
    let temp = path.with_extension("part");
    ensure!(!temp.is_symlink(), "媒体临时缓存不能是链接");
    let mut file = File::create(&temp)?;
    file.write_all(data)?;
    file.sync_all()?;
    drop(file);
    commit(&temp, path)
}
fn verified(path: &Path, size: u64, hashes: &[String]) -> bool {
    fs::metadata(path).is_ok_and(|m| m.is_file() && m.len() == size)
        && hash_file::<Md5>(path).is_ok_and(|digest| hashes.contains(&digest))
}
fn copy_verified(
    source: &Path,
    destination: &Path,
    size: Option<u64>,
    hashes: &[String],
) -> Result<()> {
    let temporary = destination.with_extension("part");
    ensure!(
        !temporary.is_symlink() && !destination.is_symlink(),
        "媒体暂存文件不能是链接"
    );
    fs::copy(source, &temporary)?;
    let length = fs::metadata(&temporary)?.len();
    if length == 0
        || length > MAX_FILE
        || size.is_some_and(|expected| length != expected)
        || !hash_file::<Md5>(&temporary).is_ok_and(|digest| hashes.contains(&digest))
    {
        let _ = fs::remove_file(&temporary);
        return Err(pending("媒体缓存复制时发生变化，完整校验后会自动重试"));
    }
    commit(&temporary, destination)
}
fn validate_name(name: &str) -> Result<()> {
    let base = name.split('.').next().unwrap_or("").to_uppercase();
    ensure!(
        !name.is_empty()
            && name.len() <= 512
            && !name.ends_with([' ', '.'])
            && !name.chars().any(|c| c < ' ' || "\\/:*?\"<>|".contains(c))
            && name != "."
            && name != ".."
            && !matches!(base.as_str(), "CON" | "PRN" | "AUX" | "NUL")
            && !(base.len() == 4
                && (base.starts_with("COM") || base.starts_with("LPT"))
                && matches!(base.as_bytes()[3], b'1'..=b'9')),
        "附件原文件名无效"
    );
    Ok(())
}
fn xml_document(content: &str) -> Result<roxmltree::Document<'_>> {
    ensure!(
        content.len() <= 8_000_000
            && !content.to_ascii_uppercase().contains("<!DOCTYPE")
            && !content.to_ascii_uppercase().contains("<!ENTITY"),
        "消息 XML 超过安全解析范围"
    );
    let start = content.find('<').context("消息 XML 缺失")?;
    Ok(roxmltree::Document::parse(&content[start..]).context("消息 XML 尚未完整解析")?)
}
fn text_at<'a, 'input>(root: roxmltree::Node<'a, 'input>, tag: &str) -> &'a str {
    root.descendants()
        .find(|node| node.has_tag_name(tag))
        .and_then(|node| node.text())
        .unwrap_or("")
}
fn xor_from_tail(path: &Path) -> u8 {
    let stem = path.file_stem().unwrap_or_default().to_string_lossy();
    let base = stem.trim_end_matches("_h").trim_end_matches("_t");
    for candidate in [
        path.with_file_name(format!("{base}_t.dat")),
        path.with_file_name(format!("{base}_h.dat")),
        path.to_path_buf(),
    ] {
        if let Ok(mut file) = File::open(candidate) {
            if let Ok(size) = file.metadata().map(|m| m.len()) {
                let back = size.min(34) as usize;
                if file.seek(SeekFrom::End(-(back as i64))).is_err() {
                    continue;
                }
                let mut tail = vec![0; back];
                if file.read_exact(&mut tail).is_err() {
                    continue;
                }
                for pair in tail.windows(2).rev() {
                    let key = pair[0] ^ 0xff;
                    if pair[1] ^ key == 0xd9 {
                        return key;
                    }
                }
            }
        }
    }
    0x88
}
fn scan_image_key(data: &[u8], probe: &[u8]) -> Option<[u8; 16]> {
    for word in data
        .split(|byte| !byte.is_ascii_alphanumeric())
        .filter(|word| word.len() >= 16)
    {
        let key: [u8; 16] = word[..16].try_into().unwrap();
        if picture::verifies_key(&key, probe) {
            return Some(key);
        }
    }
    for parity in 0..2 {
        let mut candidate = Vec::with_capacity(32);
        for pair in data.get(parity..).unwrap_or(&[]).chunks_exact(2) {
            if pair[0].is_ascii_alphanumeric() && pair[1] == 0 {
                if candidate.len() < 32 {
                    candidate.push(pair[0]);
                }
            } else {
                if candidate.len() >= 16 {
                    let key: [u8; 16] = candidate[..16].try_into().unwrap();
                    if picture::verifies_key(&key, probe) {
                        return Some(key);
                    }
                }
                candidate.clear();
            }
        }
        if candidate.len() >= 16 {
            let key: [u8; 16] = candidate[..16].try_into().unwrap();
            if picture::verifies_key(&key, probe) {
                return Some(key);
            }
        }
    }
    None
}
fn cached_picture(folder: &Path) -> Option<PathBuf> {
    for entry in fs::read_dir(folder).ok()?.filter_map(|entry| entry.ok()) {
        let path = entry.path();
        if !path.is_file()
            || path.is_symlink()
            || !path
                .file_name()
                .is_some_and(|name| name.to_string_lossy().starts_with("image."))
        {
            continue;
        }
        if let Ok(data) = read_limited(&path, MAX_FILE) {
            if picture::validate(&data, 12000).is_ok() {
                return Some(path);
            }
        }
    }
    None
}
fn emoji_uri(url: &str) -> Result<ureq::http::Uri> {
    ensure!(!url.contains('#'), "微信表情 CDN 地址无效");
    let uri: ureq::http::Uri = url.parse().context("微信表情 CDN 地址无效")?;
    let scheme = uri.scheme_str().unwrap_or("");
    let host = uri.host().unwrap_or("").to_lowercase();
    ensure!(
        matches!(scheme, "http" | "https")
            && matches!(
                host.as_str(),
                "vweixinf.tc.qq.com" | "wxapp.tc.qq.com" | "mmbiz.qpic.cn" | "mmbiz.qlogo.cn"
            )
            && !uri.authority().is_some_and(|a| a.as_str().contains('@'))
            && uri
                .port_u16()
                .is_none_or(|port| port == if scheme == "http" { 80 } else { 443 }),
        "微信表情资源地址不在允许的腾讯 CDN 范围内"
    );
    Ok(uri)
}
fn emoji_resource(url: &str) -> Result<Vec<u8>> {
    let start = Instant::now();
    let mut current = url.to_owned();
    let config = ureq::Agent::config_builder()
        .proxy(None)
        .max_redirects(0)
        .timeout_global(Some(Duration::from_secs(12)))
        .build();
    let agent = ureq::Agent::new_with_config(config);
    for _ in 0..6 {
        let uri = emoji_uri(&current)?;
        ensure!(
            start.elapsed() < Duration::from_secs(12),
            "微信表情下载超时"
        );
        let mut response = agent
            .get(current.as_str())
            .call()
            .map_err(|_| pending("微信表情资源暂时无法下载，会自动重试"))?;
        if response.status().is_redirection() {
            let location = response
                .headers()
                .get("location")
                .and_then(|v| v.to_str().ok())
                .context("微信表情重定向无效")?;
            current = if location.starts_with("http://") || location.starts_with("https://") {
                location.to_owned()
            } else if location.starts_with("//") {
                format!("{}:{location}", uri.scheme_str().unwrap())
            } else if location.starts_with('/') {
                format!(
                    "{}://{}{location}",
                    uri.scheme_str().unwrap(),
                    uri.authority().unwrap()
                )
            } else {
                format!(
                    "{}://{}{}/{location}",
                    uri.scheme_str().unwrap(),
                    uri.authority().unwrap(),
                    uri.path()
                        .rsplit_once('/')
                        .map(|(base, _)| base)
                        .unwrap_or("")
                )
            };
            continue;
        }
        ensure!(response.status().is_success(), "微信表情下载未成功");
        let mut data = Vec::new();
        response
            .body_mut()
            .as_reader()
            .take(MAX_IMAGE + 1)
            .read_to_end(&mut data)?;
        ensure!(
            !data.is_empty() && data.len() as u64 <= MAX_IMAGE,
            "微信表情原资源为空或超过 10 MiB"
        );
        return Ok(data);
    }
    bail!("微信表情重定向次数过多")
}

#[cfg(test)]
mod tests;
