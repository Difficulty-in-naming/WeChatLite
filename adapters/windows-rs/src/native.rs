//! Version-gated access to the existing native WeChat bridge. No Python or shell processes.
use anyhow::{Context, Result, anyhow, ensure};
use rand::RngCore;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    borrow::Cow,
    collections::BTreeSet,
    fmt,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    net::TcpListener,
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};

pub const VERSION: &str = "4.1.15.13";
const CLIENT_HASH: &str = "10f8e995453e2da46d4f2b5080cd6da1f13cc5147746adc119ceae38cb039de5";
const BRIDGE_HASH: &str = "8b6d16d649abe493f1cac557dff2e4ca4e3f16746d132549397bc2cc9e110b6d";
const BRIDGE: &[u8] = include_bytes!("../vendor/WeChatLocal.dll");
const MAX_MEDIA: u64 = 512 * 1024 * 1024;

#[derive(Debug)]
pub struct NativeHttpError {
    pub code: u16,
}

impl NativeHttpError {
    pub fn definite_rejection(&self) -> bool {
        matches!(self.code, 400 | 401 | 404 | 413)
    }
}

impl fmt::Display for NativeHttpError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(out, "本机微信组件返回 HTTP {}", self.code)
    }
}

impl std::error::Error for NativeHttpError {}

pub struct NativeClient {
    root: PathBuf,
    pub pid: Option<u32>,
    token: String,
    port: u16,
    allowed: BTreeSet<String>,
    media_root: Option<PathBuf>,
    agent: ureq::Agent,
    binary: Cow<'static, [u8]>,
    binary_hash: &'static str,
    extra_config: Value,
}

impl NativeClient {
    pub fn new(root: PathBuf) -> Self {
        Self::with_binary(root, BRIDGE, BRIDGE_HASH, json!({}))
    }

    pub fn with_binary(
        root: PathBuf,
        binary: impl Into<Cow<'static, [u8]>>,
        binary_hash: &'static str,
        extra_config: Value,
    ) -> Self {
        Self {
            root,
            pid: None,
            token: String::new(),
            port: 0,
            allowed: BTreeSet::new(),
            media_root: None,
            agent: ureq::Agent::config_builder()
                .proxy(None)
                .max_redirects(0)
                .max_redirects_will_error(false)
                .http_status_as_error(false)
                .timeout_global(Some(Duration::from_secs(20)))
                .build()
                .new_agent(),
            binary: binary.into(),
            binary_hash,
            extra_config,
        }
    }

    pub fn connect(&mut self, groups: &[String], media_root: &Path) -> Result<Value> {
        let groups = normalized_groups(groups)?;
        ensure!(!groups.is_empty(), "需要明确选择可同步的微信群");
        ensure!(usize::BITS == 64, "本机微信组件要求 64 位程序");
        ensure!(
            hex::encode(Sha256::digest(self.binary.as_ref())) == self.binary_hash,
            "内置微信组件指纹不匹配"
        );
        let (pid, loaded) = platform::main_process()?;
        fs::create_dir_all(&self.root)?;
        self.ensure_root()?;
        let root = self.root.canonicalize()?;
        let directory = root.join(&self.binary_hash[..16]);
        fs::create_dir_all(&directory)?;
        ensure!(
            directory.canonicalize()?.starts_with(&root),
            "本机组件缓存目录越界"
        );
        let destination = directory.join(format!("WeChatLocal_{}.dll", &self.binary_hash[..16]));
        let already_loaded = loaded
            .iter()
            .any(|module| same_path(&module.path, &destination));
        if destination.exists() {
            ensure!(
                destination.canonicalize()?.starts_with(&root),
                "本机组件文件越界"
            );
        }
        if !destination.is_file() || file_hash(&destination)? != self.binary_hash {
            ensure!(!already_loaded, "已加载的本机组件文件指纹不匹配");
            write_atomic(&destination, self.binary.as_ref())?;
        }
        ensure!(
            file_hash(&destination)? == self.binary_hash,
            "本机组件缓存校验失败"
        );
        let config_path = directory.join("native.json");
        let mut config: Value = if config_path.exists() {
            ensure!(
                config_path.canonicalize()?.starts_with(&root),
                "本机组件配置越界"
            );
            serde_json::from_reader(File::open(&config_path)?)?
        } else {
            json!({})
        };
        ensure!(config.is_object(), "本机组件配置无效");
        for (key, value) in self
            .extra_config
            .as_object()
            .context("本机组件额外配置无效")?
        {
            ensure!(
                key == "self_wxid"
                    || (key == "capability"
                        && matches!(value.as_str(), Some("recall" | "quote_text"))),
                "本机组件额外配置字段无效"
            );
            config[key] = value.clone();
        }
        self.token = config["token"]
            .as_str()
            .filter(|value| valid_token(value))
            .map(str::to_owned)
            .unwrap_or_else(random_token);
        self.port = if already_loaded {
            let port = config["port"]
                .as_u64()
                .context("已加载组件的本地端口缺失")?;
            ensure!((1..=65535).contains(&port), "本机组件端口无效");
            port as u16
        } else {
            TcpListener::bind(("127.0.0.1", 0))?.local_addr()?.port()
        };
        fs::create_dir_all(media_root)?;
        self.media_root = Some(media_root.canonicalize()?);
        self.pid = Some(pid);
        config["token"] = json!(self.token);
        config["groups"] = json!(groups);
        config["port"] = json!(self.port);
        config["media_root"] = json!(native_path(self.media_root.as_ref().unwrap())?);
        write_atomic(&config_path, &serde_json::to_vec(&config)?)?;
        if self.extra_config.get("self_wxid").is_some() {
            let capability = self.extra_config["capability"].as_str().unwrap_or("recall");
            for old in config_files(&self.root)? {
                if same_path(&old, &config_path)
                    || !loaded.iter().any(|module| {
                        module.path.parent().is_some_and(|parent| {
                            old.parent().is_some_and(|other| same_path(parent, other))
                        })
                    })
                {
                    continue;
                }
                let previous: Value = serde_json::from_reader(File::open(&old)?)?;
                let port = previous["port"]
                    .as_u64()
                    .filter(|port| (1..=65535).contains(port))
                    .context("旧撤回组件端口无效")?;
                let token = previous["token"]
                    .as_str()
                    .filter(|token| valid_token(token))
                    .context("旧撤回组件令牌无效")?;
                let url = format!("http://127.0.0.1:{port}");
                let authorization = format!("Bearer {token}");
                let health: Value = self
                    .agent
                    .get(format!("{url}/health"))
                    .header("Authorization", &authorization)
                    .call()?
                    .body_mut()
                    .with_config()
                    .limit(4096)
                    .read_json()?;
                ensure!(
                    health["pid"] == pid
                        && health["capabilities"]
                            .as_array()
                            .is_some_and(|caps| caps.iter().any(|cap| cap == capability)),
                    "旧撤回组件身份无法核对"
                );
                let stopped: Value = self
                    .agent
                    .post(format!("{url}/stop"))
                    .header("Authorization", &authorization)
                    .send_json(json!({}))?
                    .body_mut()
                    .with_config()
                    .limit(4096)
                    .read_json()?;
                ensure!(
                    stopped["disabled"] == true,
                    "旧撤回组件未停止，未加载新版本"
                );
                shrink_config(&self.root, &old, &BTreeSet::new())?;
            }
        }
        platform::load_library(pid, &loaded, &destination)?;
        let deadline = Instant::now() + Duration::from_secs(20);
        let health = loop {
            match self.request("/health", None) {
                Ok(health) => {
                    ensure!(
                        health["pid"].as_u64() == Some(pid as u64)
                            && health["version"] == VERSION
                            && !health_has_error(&health["error"]),
                        "本机微信组件进程或版本校验未通过"
                    );
                    break health;
                }
                Err(error) => {
                    if error.downcast_ref::<NativeHttpError>().is_some()
                        || Instant::now() >= deadline
                    {
                        return Err(error.context("本机微信组件未就绪；未发送消息"));
                    }
                    thread::sleep(Duration::from_millis(200));
                }
            }
        };
        self.allowed = groups;
        self.restrict(&self.allowed.iter().cloned().collect::<Vec<_>>())?;
        // Old binary versions remain loaded until WeChat exits; keep only this config enabled.
        for path in config_files(&self.root)? {
            if !same_path(&path, &config_path) {
                shrink_config(&self.root, &path, &BTreeSet::new())?;
            }
        }
        Ok(health)
    }

    pub fn request(&self, path: &str, body: Option<&Value>) -> Result<Value> {
        ensure!(
            self.port != 0 && valid_token(&self.token),
            "本机微信组件尚未连接"
        );
        ensure!(
            path.starts_with('/') && !path.starts_with("//") && !path.contains(['\r', '\n', '#']),
            "本机微信请求路径无效"
        );
        let url = format!("http://127.0.0.1:{}{}", self.port, path);
        let authorization = format!("Bearer {}", self.token);
        let mut response = match body {
            Some(body) => self
                .agent
                .post(&url)
                .header("Authorization", &authorization)
                .header("Content-Type", "application/json")
                .send_json(body),
            None => self
                .agent
                .get(&url)
                .header("Authorization", &authorization)
                .header("Content-Type", "application/json")
                .call(),
        }
        .context("本机微信组件连接或响应中断")?;
        let code = response.status().as_u16();
        if !(200..300).contains(&code) {
            let error = anyhow::Error::new(NativeHttpError { code });
            let detail = response
                .body_mut()
                .with_config()
                .limit(4096)
                .read_json::<Value>()
                .ok()
                .and_then(|value| {
                    value["error"]
                        .as_str()
                        .filter(|value| !value.is_empty() && value.len() <= 512)
                        .map(str::to_owned)
                });
            return Err(if let Some(detail) = detail {
                error.context(detail)
            } else {
                error
            });
        }
        response
            .body_mut()
            .with_config()
            .limit(2 * 1024 * 1024)
            .read_json()
            .context("本机微信组件响应不是完整 JSON")
    }

    pub fn attach_existing(&mut self) -> Result<bool> {
        self.ensure_root()?;
        let (pid, loaded) = platform::main_process()?;
        let directory = self.root.join(&self.binary_hash[..16]);
        let destination = directory.join(format!("WeChatLocal_{}.dll", &self.binary_hash[..16]));
        if !loaded
            .iter()
            .any(|module| same_path(&module.path, &destination))
        {
            return Ok(false);
        }
        ensure!(
            file_hash(&destination)? == self.binary_hash,
            "已加载撤回组件文件指纹不匹配"
        );
        let config: Value = serde_json::from_reader(File::open(directory.join("native.json"))?)?;
        for (key, value) in self.extra_config.as_object().context("组件配置无效")? {
            ensure!(config[key] == *value, "已加载组件属于另一个微信账号");
        }
        self.token = config["token"]
            .as_str()
            .filter(|token| valid_token(token))
            .context("组件令牌无效")?
            .to_owned();
        let port = config["port"].as_u64().context("组件端口无效")?;
        ensure!((1..=65535).contains(&port), "组件端口无效");
        self.port = port as u16;
        self.pid = Some(pid);
        let health = self.request("/health", None)?;
        ensure!(
            health["pid"] == pid
                && health["version"] == VERSION
                && !health_has_error(&health["error"]),
            "已加载组件身份校验失败"
        );
        Ok(true)
    }

    pub fn send(&self, group: &str, text: &str, key: &str, atlist: &str) -> Result<Value> {
        self.check_send(group, key)?;
        ensure!(
            !text.is_empty() && text.len() <= 24_000 && !text.contains('\0'),
            "微信文本无效"
        );
        self.request(
            "/SendTextMsg",
            Some(&json!({"wxidorgid":group,"msg":text,
            "atlist":atlist,"request_id":request_id(key)})),
        )
    }

    pub fn send_media(&self, group: &str, path: &Path, kind: &str, key: &str) -> Result<Value> {
        self.check_send(group, key)?;
        ensure!(
            matches!(kind, "image" | "gif" | "file"),
            "微信附件发送类型无效"
        );
        let path = path.canonicalize()?;
        ensure!(
            path.starts_with(self.media_root.as_ref().context("微信附件目录尚未设置")?)
                && path.is_file()
                && (1..=MAX_MEDIA).contains(&path.metadata()?.len()),
            "微信附件超出目录或容量限制"
        );
        self.request(
            "/SendMediaMsg",
            Some(&json!({"wxidorgid":group,"path":native_path(&path)?,
            "kind":kind,"request_id":request_id(key)})),
        )
    }

    pub fn download_video(
        &self,
        group: &str,
        file_id: &str,
        aes_key: &str,
        length: u64,
        key: &str,
    ) -> Result<Value> {
        self.check_send(group, key)?;
        ensure!(
            !file_id.is_empty()
                && file_id.len() <= 16384
                && !aes_key.is_empty()
                && aes_key.len() <= 256
                && (1..=MAX_MEDIA).contains(&length),
            "微信视频下载参数无效"
        );
        self.request(
            "/DownloadVideo",
            Some(&json!({"group":group,"file_id":file_id,"aes_key":aes_key,
            "length":length,"request_id":request_id(key)})),
        )
    }

    pub fn is_alive(&self) -> bool {
        self.pid.is_some_and(platform::is_alive)
    }

    fn check_send(&self, group: &str, key: &str) -> Result<()> {
        ensure!(self.allowed.contains(group), "该群未授权给本机微信组件");
        ensure!(!key.is_empty() && key.len() <= 4096, "微信发送任务标识无效");
        ensure!(self.is_alive(), "微信主进程已退出，请重新连接本机微信");
        Ok(())
    }

    pub fn restrict(&mut self, groups: &[String]) -> Result<()> {
        self.ensure_root()?;
        let allowed = normalized_groups(groups)?;
        self.allowed.retain(|group| allowed.contains(group));
        let mut failure = None;
        for path in config_files(&self.root)? {
            if let Err(error) = shrink_config(&self.root, &path, &allowed) {
                failure.get_or_insert(error);
            }
        }
        failure.map_or(Ok(()), Err)
    }
    fn ensure_root(&self) -> Result<()> {
        if self.root.exists() {
            let resolved = self.root.canonicalize()?;
            let parent = self
                .root
                .parent()
                .context("组件目录缺少父目录")?
                .canonicalize()?;
            ensure!(
                resolved != parent && resolved.starts_with(parent),
                "本机组件目录越出当前适配器目录"
            );
        }
        Ok(())
    }
}

impl Drop for NativeClient {
    fn drop(&mut self) {
        if let Err(error) = self.restrict(&[]) {
            eprintln!("本机微信组件授权清理未确认：{error:#}");
        }
    }
}

pub fn verify_wechat() -> Result<()> {
    platform::main_process().map(|_| ())
}

pub fn verified_ui_process() -> Result<(u32, usize)> {
    let (pid, modules) = platform::main_process()?;
    let client = modules
        .iter()
        .find(|module| module.name == "weixin.dll")
        .context("微信主模块不可用")?;
    Ok((pid, client.base))
}

pub fn request_id(key: &str) -> String {
    hex::encode(Sha256::digest(key.as_bytes()))
}

fn normalized_groups(groups: &[String]) -> Result<BTreeSet<String>> {
    ensure!(groups.len() <= 4096, "微信群授权数量过多");
    for group in groups {
        let number = group.strip_suffix("@chatroom").context("微信群编号无效")?;
        ensure!(
            (5..=24).contains(&number.len()) && number.bytes().all(|byte| byte.is_ascii_digit()),
            "微信群编号无效"
        );
    }
    Ok(groups.iter().cloned().collect())
}

fn valid_token(token: &str) -> bool {
    token.len() == 64
        && token
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn health_has_error(value: &Value) -> bool {
    match value {
        Value::Null => false,
        Value::Bool(value) => *value,
        Value::Number(value) => value.as_f64() != Some(0.0),
        Value::String(value) => !value.is_empty(),
        Value::Array(value) => !value.is_empty(),
        Value::Object(value) => !value.is_empty(),
    }
}

fn random_token() -> String {
    let mut bytes = [0u8; 32];
    rand::rng().fill_bytes(&mut bytes);
    hex::encode(bytes)
}

fn file_hash(path: &Path) -> Result<String> {
    let mut hash = Sha256::new();
    let mut input = File::open(path)?;
    let mut buffer = [0u8; 65536];
    loop {
        let length = input.read(&mut buffer)?;
        if length == 0 {
            break;
        }
        hash.update(&buffer[..length]);
    }
    Ok(hex::encode(hash.finalize()))
}

fn native_path(path: &Path) -> Result<String> {
    let text = path.to_str().context("微信媒体路径包含无效字符")?;
    if let Some(unc) = text.strip_prefix(r"\\?\UNC\") {
        return Ok(format!(r"\\{unc}"));
    }
    Ok(text.strip_prefix(r"\\?\").unwrap_or(text).to_owned())
}

fn same_path(left: &Path, right: &Path) -> bool {
    match (left.canonicalize(), right.canonicalize()) {
        (Ok(left), Ok(right)) => left
            .to_string_lossy()
            .eq_ignore_ascii_case(&right.to_string_lossy()),
        _ => false,
    }
}

fn write_atomic(path: &Path, content: &[u8]) -> Result<()> {
    let temp = path.with_extension(format!("{}.tmp", &random_token()[..16]));
    let result = (|| -> Result<()> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(content)?;
        file.flush()?;
        drop(file);
        for attempt in 0..5 {
            match fs::rename(&temp, path) {
                Ok(()) => return Ok(()),
                Err(error) if attempt == 4 => return Err(error.into()),
                Err(_) => thread::sleep(Duration::from_millis(10)),
            }
        }
        unreachable!()
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

fn config_files(root: &Path) -> Result<Vec<PathBuf>> {
    if !root.exists() {
        return Ok(Vec::new());
    }
    let mut result = Vec::new();
    if root.join("native.json").is_file() {
        result.push(root.join("native.json"));
    }
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() && entry.path().join("native.json").is_file() {
            result.push(entry.path().join("native.json"));
        }
    }
    Ok(result)
}

fn shrink_config(root: &Path, path: &Path, allowed: &BTreeSet<String>) -> Result<()> {
    ensure!(
        path.canonicalize()?.starts_with(root.canonicalize()?),
        "本机组件配置越界，未修改外部文件"
    );
    let mut value: Value = serde_json::from_reader(File::open(path)?)?;
    ensure!(value.is_object(), "本机组件配置无效，无法确认授权已清空");
    let remaining: BTreeSet<String> = value["groups"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|group| allowed.contains(*group))
        .map(str::to_owned)
        .collect();
    if value["groups"] != json!(remaining) {
        value["groups"] = json!(remaining);
        write_atomic(path, &serde_json::to_vec(&value)?)?;
    }
    Ok(())
}

#[derive(Clone)]
struct Module {
    name: String,
    base: usize,
    path: PathBuf,
}

fn validate_client(path: &Path, digest: &str) -> Result<()> {
    ensure!(
        path.parent()
            .and_then(Path::file_name)
            .and_then(|name| name.to_str())
            == Some(VERSION)
            && digest == CLIENT_HASH,
        "微信版本或 DLL 指纹不匹配，只适配未经修改的 4.1.15.13"
    );
    Ok(())
}

fn worker_command_line(command: &str) -> bool {
    command
        .split_whitespace()
        .map(|piece| piece.trim_matches('"'))
        .any(|piece| piece.starts_with("--type=") || piece == "--crashpad-handler")
}

#[cfg(windows)]
mod platform {
    use super::*;
    use std::{
        ffi::{OsString, c_void},
        mem::{size_of, zeroed},
        os::windows::ffi::{OsStrExt, OsStringExt},
        ptr,
    };
    use windows_sys::Win32::{
        Foundation::{
            CloseHandle, ERROR_BAD_LENGTH, GetLastError, HANDLE, INVALID_HANDLE_VALUE,
            WAIT_OBJECT_0, WAIT_TIMEOUT,
        },
        System::{
            Diagnostics::{
                Debug::{ReadProcessMemory, WriteProcessMemory},
                ToolHelp::{
                    CreateToolhelp32Snapshot, MODULEENTRY32W, Module32FirstW, Module32NextW,
                    PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPMODULE,
                    TH32CS_SNAPMODULE32, TH32CS_SNAPPROCESS,
                },
            },
            LibraryLoader::{
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, GetModuleFileNameW,
                GetModuleHandleExW, GetModuleHandleW, GetProcAddress,
            },
            Memory::{
                MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE, VirtualAllocEx, VirtualFreeEx,
            },
            Threading::{
                CreateRemoteThread, OpenProcess, PROCESS_CREATE_THREAD, PROCESS_QUERY_INFORMATION,
                PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_OPERATION, PROCESS_VM_READ,
                PROCESS_VM_WRITE, WaitForSingleObject,
            },
        },
    };

    struct Handle(HANDLE);
    impl Handle {
        fn new(handle: HANDLE) -> Result<Self> {
            ensure!(
                !handle.is_null() && handle != INVALID_HANDLE_VALUE,
                "{}",
                std::io::Error::last_os_error()
            );
            Ok(Self(handle))
        }
    }
    impl Drop for Handle {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }

    fn wide_string(value: &[u16]) -> OsString {
        OsString::from_wide(
            &value[..value
                .iter()
                .position(|value| *value == 0)
                .unwrap_or(value.len())],
        )
    }

    fn modules(pid: u32) -> Result<Vec<Module>> {
        let snapshot = (0..5)
            .find_map(|_| {
                let handle = unsafe {
                    CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
                };
                if handle != INVALID_HANDLE_VALUE {
                    return Some(Ok(Handle(handle)));
                }
                if unsafe { GetLastError() } != ERROR_BAD_LENGTH {
                    return Some(Err(anyhow!(std::io::Error::last_os_error())));
                }
                thread::sleep(Duration::from_millis(10));
                None
            })
            .context("微信模块列表持续变化，未加载组件")??;
        let mut entry: MODULEENTRY32W = unsafe { zeroed() };
        entry.dwSize = size_of::<MODULEENTRY32W>() as u32;
        let mut more = unsafe { Module32FirstW(snapshot.0, &mut entry) };
        let mut result = Vec::new();
        while more != 0 {
            result.push(Module {
                name: wide_string(&entry.szModule)
                    .to_string_lossy()
                    .to_lowercase(),
                base: entry.modBaseAddr as usize,
                path: PathBuf::from(wide_string(&entry.szExePath)),
            });
            more = unsafe { Module32NextW(snapshot.0, &mut entry) };
        }
        ensure!(!result.is_empty(), "无法读取微信主进程的模块列表");
        Ok(result)
    }

    #[repr(C)]
    struct ProcessBasicInformation {
        reserved1: *mut c_void,
        peb: *mut c_void,
        reserved2: [*mut c_void; 2],
        pid: usize,
        reserved3: *mut c_void,
    }

    #[link(name = "ntdll")]
    unsafe extern "system" {
        fn NtQueryInformationProcess(
            process: HANDLE,
            class: u32,
            buffer: *mut c_void,
            length: u32,
            returned: *mut u32,
        ) -> i32;
    }

    fn read_memory(process: HANDLE, address: usize, output: &mut [u8]) -> Result<()> {
        let mut count = 0;
        ensure!(
            unsafe {
                ReadProcessMemory(
                    process,
                    address as *const c_void,
                    output.as_mut_ptr().cast(),
                    output.len(),
                    &mut count,
                )
            } != 0
                && count == output.len(),
            "微信进程信息无法完整读取"
        );
        Ok(())
    }

    fn command_line(pid: u32) -> Result<String> {
        ensure!(usize::BITS == 64, "微信进程读取要求64位程序");
        let process = Handle::new(unsafe {
            OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 0, pid)
        })?;
        let mut basic: ProcessBasicInformation = unsafe { zeroed() };
        let status = unsafe {
            NtQueryInformationProcess(
                process.0,
                0,
                (&mut basic as *mut ProcessBasicInformation).cast(),
                size_of::<ProcessBasicInformation>() as u32,
                ptr::null_mut(),
            )
        };
        ensure!(
            status >= 0 && !basic.peb.is_null(),
            "微信进程基本信息不可读取"
        );
        let mut pointer = [0u8; 8];
        read_memory(process.0, basic.peb as usize + 0x20, &mut pointer)?;
        let parameters = u64::from_le_bytes(pointer) as usize;
        ensure!(parameters != 0, "微信进程参数不可读取");
        let mut command = [0u8; 16];
        read_memory(process.0, parameters + 0x70, &mut command)?;
        let length = u16::from_le_bytes(command[..2].try_into().unwrap()) as usize;
        let address = u64::from_le_bytes(command[8..].try_into().unwrap()) as usize;
        ensure!(
            length > 0 && length % 2 == 0 && address != 0,
            "微信命令行信息无效"
        );
        let mut data = vec![0u8; length];
        read_memory(process.0, address, &mut data)?;
        Ok(String::from_utf16_lossy(
            &data
                .chunks_exact(2)
                .map(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]))
                .collect::<Vec<_>>(),
        ))
    }

    pub(super) fn main_process() -> Result<(u32, Vec<Module>)> {
        ensure!(usize::BITS == 64, "本机微信组件要求64位程序");
        let snapshot = Handle::new(unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) })?;
        let mut entry: PROCESSENTRY32W = unsafe { zeroed() };
        entry.dwSize = size_of::<PROCESSENTRY32W>() as u32;
        let mut more = unsafe { Process32FirstW(snapshot.0, &mut entry) };
        let mut candidates = Vec::new();
        while more != 0 {
            if wide_string(&entry.szExeFile)
                .to_string_lossy()
                .eq_ignore_ascii_case("Weixin.exe")
            {
                let pid = entry.th32ProcessID;
                if let Ok(loaded) = modules(pid) {
                    if loaded.iter().any(|module| module.name == "weixin.dll")
                        && !worker_command_line(&command_line(pid)?)
                    {
                        candidates.push((pid, loaded));
                    }
                }
            }
            more = unsafe { Process32NextW(snapshot.0, &mut entry) };
        }
        ensure!(
            candidates.len() == 1,
            "需要恰好一个已登录的微信主进程，未加载组件"
        );
        let (pid, loaded) = candidates.remove(0);
        let client = &loaded
            .iter()
            .find(|module| module.name == "weixin.dll")
            .unwrap()
            .path;
        validate_client(client, &file_hash(client)?)?;
        Ok((pid, loaded))
    }

    pub(super) fn is_alive(pid: u32) -> bool {
        let Ok(process) = Handle::new(unsafe {
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | 0x00100000, 0, pid)
        }) else {
            return false;
        };
        unsafe { WaitForSingleObject(process.0, 0) == WAIT_TIMEOUT }
    }

    pub(super) fn load_library(pid: u32, loaded: &[Module], path: &Path) -> Result<()> {
        if loaded.iter().any(|module| same_path(&module.path, path)) {
            return Ok(());
        }
        let kernel_name: Vec<u16> = "kernel32.dll\0".encode_utf16().collect();
        let local_kernel = unsafe { GetModuleHandleW(kernel_name.as_ptr()) };
        ensure!(!local_kernel.is_null(), "无法定位Windows模块加载器");
        let loader = unsafe { GetProcAddress(local_kernel, c"LoadLibraryW".as_ptr().cast()) }
            .context("无法定位LoadLibraryW")?;
        let mut containing = ptr::null_mut();
        ensure!(
            unsafe {
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    loader as *const u16,
                    &mut containing,
                )
            } != 0,
            "无法定位Windows模块加载器所属模块"
        );
        let mut name = vec![0u16; 32768];
        let count = unsafe { GetModuleFileNameW(containing, name.as_mut_ptr(), name.len() as u32) }
            as usize;
        ensure!(count > 0 && count < name.len(), "模块加载器路径无效");
        let local_path = PathBuf::from(OsString::from_wide(&name[..count]));
        let name = local_path
            .file_name()
            .context("模块加载器路径无效")?
            .to_string_lossy()
            .to_lowercase();
        let remote_base = loaded
            .iter()
            .find(|module| module.name == name)
            .context("微信缺少模块加载器所属模块")?
            .base;
        let address = remote_base
            .checked_add(loader as usize - containing as usize)
            .context("模块加载器地址溢出")?;
        let process = Handle::new(unsafe {
            OpenProcess(
                PROCESS_CREATE_THREAD
                    | PROCESS_QUERY_INFORMATION
                    | PROCESS_VM_OPERATION
                    | PROCESS_VM_READ
                    | PROCESS_VM_WRITE,
                0,
                pid,
            )
        })?;
        let path: Vec<u16> = path
            .canonicalize()?
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect();
        let bytes = path.len() * size_of::<u16>();
        let allocation = unsafe {
            VirtualAllocEx(
                process.0,
                ptr::null(),
                bytes,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE,
            )
        };
        ensure!(!allocation.is_null(), "微信组件路径无法分配");
        let mut thread_started = false;
        let mut completed = false;
        let result = (|| -> Result<()> {
            let mut written = 0;
            ensure!(
                unsafe {
                    WriteProcessMemory(
                        process.0,
                        allocation,
                        path.as_ptr().cast(),
                        bytes,
                        &mut written,
                    )
                } != 0
                    && written == bytes,
                "微信组件路径写入不完整，未执行加载"
            );
            let start: unsafe extern "system" fn(*mut c_void) -> u32 =
                unsafe { std::mem::transmute(address) };
            let thread = Handle::new(unsafe {
                CreateRemoteThread(
                    process.0,
                    ptr::null(),
                    0,
                    Some(start),
                    allocation,
                    0,
                    ptr::null_mut(),
                )
            })?;
            thread_started = true;
            ensure!(
                unsafe { WaitForSingleObject(thread.0, 30000) } == WAIT_OBJECT_0,
                "微信组件加载等待超时，未发送任何消息"
            );
            completed = true;
            Ok(())
        })();
        // A timed-out loader thread may still read the remote argument; do not free it underneath that thread.
        if completed || !thread_started {
            unsafe {
                VirtualFreeEx(process.0, allocation, 0, MEM_RELEASE);
            }
        }
        result?;
        ensure!(
            modules(pid)?.iter().any(|module| same_path(
                &module.path,
                &PathBuf::from(OsString::from_wide(&path[..path.len() - 1]))
            )),
            "微信未成功加载本机组件"
        );
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    const GROUP: &str = "123456@chatroom";

    fn config(root: &Path, relative: &str, groups: &[&str]) -> PathBuf {
        let path = root.join(relative);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(
            &path,
            serde_json::to_vec(&json!({"groups":groups,"token":"fixture-token",
            "port":12345,"media_root":"fixture media"}))
            .unwrap(),
        )
        .unwrap();
        path
    }

    fn read_config(path: &Path) -> Value {
        serde_json::from_reader(File::open(path).unwrap()).unwrap()
    }

    fn local_response(code: u16, body: &'static str) -> (u16, thread::JoinHandle<(String, Value)>) {
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        let task = thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            let mut reader = BufReader::new(&mut stream);
            let mut headers = String::new();
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                if line == "\r\n" {
                    break;
                }
                assert!(!line.is_empty(), "fixture request ended before headers");
                headers.push_str(&line);
            }
            let length = headers
                .lines()
                .find_map(|line| {
                    let (name, value) = line.split_once(':')?;
                    name.eq_ignore_ascii_case("content-length")
                        .then(|| value.trim().parse::<usize>().unwrap())
                })
                .unwrap_or(0);
            let mut bytes = vec![0u8; length];
            reader.read_exact(&mut bytes).unwrap();
            drop(reader);
            write!(stream, "HTTP/1.1 {code} Fixture\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len()).unwrap();
            (
                headers,
                if bytes.is_empty() {
                    Value::Null
                } else {
                    serde_json::from_slice(&bytes).unwrap()
                },
            )
        });
        (port, task)
    }

    fn fixture_client(root: &Path, port: u16) -> NativeClient {
        let mut client = NativeClient::new(root.to_owned());
        client.port = port;
        client.token = "f".repeat(64);
        client.pid = Some(std::process::id());
        client.allowed.insert(GROUP.into());
        client
    }

    #[test]
    fn request_id_matches_existing_native_protocol() {
        assert_eq!(
            request_id("hello"),
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
        );
        assert_eq!(
            request_id("消息:0"),
            hex::encode(Sha256::digest("消息:0".as_bytes()))
        );
        assert_ne!(request_id("消息:0"), request_id("消息:1"));
    }

    #[test]
    fn bundled_binary_and_wechat_version_are_pinned() {
        assert_eq!(hex::encode(Sha256::digest(BRIDGE)), BRIDGE_HASH);
        assert!(validate_client(Path::new("fixture/4.1.15.13/Weixin.dll"), CLIENT_HASH).is_ok());
        assert!(validate_client(Path::new("fixture/4.1.15.14/Weixin.dll"), CLIENT_HASH).is_err());
        assert!(
            validate_client(Path::new("fixture/4.1.15.13/Weixin.dll"), &"0".repeat(64)).is_err()
        );
    }

    #[test]
    fn binary_fingerprint_failure_precedes_all_process_access() {
        let temp = tempfile::tempdir().unwrap();
        let mut client = NativeClient::with_binary(
            temp.path().join("native-actions"),
            b"untrusted replacement".as_slice(),
            BRIDGE_HASH,
            json!({}),
        );
        let error = client.connect(&[GROUP.into()], temp.path()).unwrap_err();
        assert_eq!(error.to_string(), "内置微信组件指纹不匹配");
        assert!(!client.root.exists());
    }

    #[test]
    fn worker_processes_are_excluded_before_loading() {
        assert!(worker_command_line(
            r#""C:\Program Files\Weixin.exe" --type=renderer --foo"#
        ));
        assert!(worker_command_line(r#"Weixin.exe "--crashpad-handler""#));
        assert!(!worker_command_line(
            r#""C:\Program Files\Weixin.exe" --lang=zh-CN"#
        ));
    }

    #[test]
    fn policies_are_explicit_and_empty_policy_needs_no_process_access() {
        assert!(normalized_groups(&[]).unwrap().is_empty());
        assert_eq!(
            normalized_groups(&[GROUP.into(), GROUP.into()])
                .unwrap()
                .len(),
            1
        );
        for group in [
            "",
            "all",
            "wxid_x",
            "1234@chatroom",
            "123456@chatroom/other",
        ] {
            assert!(normalized_groups(&[group.into()]).is_err());
        }
        let temp = tempfile::tempdir().unwrap();
        let mut client = NativeClient::new(temp.path().join("absent"));
        client.restrict(&[]).unwrap();
        assert!(!client.root.exists());
        assert!(client.connect(&[], temp.path()).is_err());
        assert!(client.send(GROUP, "fixture", "key", "").is_err());
    }

    #[test]
    fn restrict_only_shrinks_own_configs_and_preserves_secrets_and_paths() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("native");
        let first = config(&root, "native.json", &[GROUP, "987654@chatroom"]);
        let old = config(&root, "old-hash/native.json", &[GROUP, "987654@chatroom"]);
        let other = config(temp.path(), "other-service/native.json", &[GROUP]);
        let mut client = NativeClient::new(root);
        client.allowed = [GROUP.to_owned(), "987654@chatroom".into()]
            .into_iter()
            .collect();
        client.restrict(&[GROUP.into()]).unwrap();
        for path in [&first, &old] {
            let value = read_config(path);
            assert_eq!(value["groups"], json!([GROUP]));
            assert_eq!(value["token"], "fixture-token");
            assert_eq!(value["port"], 12345);
            assert_eq!(value["media_root"], "fixture media");
        }
        client.restrict(&[]).unwrap();
        assert!(client.allowed.is_empty());
        assert_eq!(read_config(&first)["groups"], json!([]));
        assert_eq!(read_config(&old)["groups"], json!([]));
        assert_eq!(read_config(&other)["groups"], json!([GROUP]));
    }

    #[test]
    fn dropping_client_revokes_own_previous_binary_permissions() {
        let temp = tempfile::tempdir().unwrap();
        let path = config(temp.path(), "old-version/native.json", &[GROUP]);
        {
            let _client = NativeClient::new(temp.path().to_owned());
        }
        assert_eq!(read_config(&path)["groups"], json!([]));
    }

    #[test]
    fn transport_never_uses_environment_proxies_or_redirects() {
        let temp = tempfile::tempdir().unwrap();
        let client = NativeClient::new(temp.path().to_owned());
        assert!(client.agent.config().proxy().is_none());
        assert_eq!(client.agent.config().max_redirects(), 0);
    }

    #[test]
    fn local_http_status_is_preserved_for_send_classification() {
        let temp = tempfile::tempdir().unwrap();
        for status in [400, 401, 404, 413, 500, 302] {
            let (port, response) = local_response(status, "{}");
            let client = fixture_client(temp.path(), port);
            let error = client.request("/health", None).unwrap_err();
            let http = error.downcast_ref::<NativeHttpError>().unwrap();
            assert_eq!(http.code, status);
            assert_eq!(
                http.definite_rejection(),
                matches!(status, 400 | 401 | 404 | 413)
            );
            response.join().unwrap();
        }
    }

    #[test]
    fn text_send_builds_stable_id_and_mentions_against_a_fake_http_server_only() {
        let temp = tempfile::tempdir().unwrap();
        let (port, response) = local_response(200, r#"{"status":"queued","ret":0}"#);
        let client = fixture_client(temp.path(), port);
        let result = client
            .send(GROUP, "@成员\u{2005}你好", "fixture:0", "wxid_fixture")
            .unwrap();
        assert_eq!(result["status"], "queued");
        let (headers, body) = response.join().unwrap();
        assert!(headers.starts_with("POST /SendTextMsg HTTP/1.1"));
        assert!(
            headers
                .to_lowercase()
                .contains(&format!("authorization: bearer {}", "f".repeat(64)))
        );
        assert_eq!(body["wxidorgid"], GROUP);
        assert_eq!(body["msg"], "@成员\u{2005}你好");
        assert_eq!(body["atlist"], "wxid_fixture");
        assert_eq!(body["request_id"], request_id("fixture:0"));
    }

    #[test]
    fn health_error_flag_accepts_zero_but_rejects_reported_errors() {
        for value in [
            Value::Null,
            json!(false),
            json!(0),
            json!(""),
            json!([]),
            json!({}),
        ] {
            assert!(!health_has_error(&value));
        }
        for value in [json!(true), json!(1), json!("not ready")] {
            assert!(health_has_error(&value));
        }
    }

    #[test]
    fn canonical_windows_media_paths_keep_native_compatible_prefixes() {
        assert_eq!(
            native_path(Path::new(r"\\?\C:\fixture\视频.mp4")).unwrap(),
            r"C:\fixture\视频.mp4"
        );
        assert_eq!(
            native_path(Path::new(r"\\?\UNC\server\share\file.gif")).unwrap(),
            r"\\server\share\file.gif"
        );
    }
}

#[cfg(not(windows))]
mod platform {
    use super::*;
    pub(super) fn main_process() -> Result<(u32, Vec<Module>)> {
        anyhow::bail!("此适配器仅支持本机Windows微信")
    }
    pub(super) fn is_alive(_: u32) -> bool {
        false
    }
    pub(super) fn load_library(_: u32, _: &[Module], _: &Path) -> Result<()> {
        anyhow::bail!("此适配器仅支持Windows")
    }
}
