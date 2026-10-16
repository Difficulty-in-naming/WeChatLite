pub mod platform;

use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs::{self, File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

pub const MAGIC: &[u8; 8] = b"WLPKG01\n";
pub const FOOTER: &[u8; 8] = b"WLPEND1\n";
const MAX_PAYLOAD: u64 = 2 * 1024 * 1024 * 1024;
const READY: &str = ".portable-ready";

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Entry {
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub role: String,
    pub entry: String,
    pub files: Vec<Entry>,
}

pub struct Package {
    pub manifest: Manifest,
    pub hash: String,
    source: Mutex<File>,
    body_offset: u64,
}

pub fn hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn valid_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

pub fn validate_path(name: &str) -> Result<()> {
    ensure!(
        !name.is_empty() && name.len() <= 240 && !name.starts_with('/'),
        "invalid package path"
    );
    ensure!(
        !name.contains(['\\', ':', '\0']) && !name.chars().any(char::is_control),
        "invalid package path"
    );
    for part in name.split('/') {
        ensure!(
            !part.is_empty() && part != "." && part != ".." && !part.ends_with(['.', ' ']),
            "invalid package component"
        );
        ensure!(
            !part.contains(['<', '>', '"', '|', '?', '*']),
            "invalid Windows filename"
        );
        let stem = part.split('.').next().unwrap().to_ascii_uppercase();
        let reserved = matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL" | "CLOCK$")
            || ["COM", "LPT"].iter().any(|prefix| {
                stem.strip_prefix(prefix).is_some_and(|n| {
                    matches!(
                        n,
                        "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
                    )
                })
            });
        ensure!(!reserved, "reserved Windows filename");
    }
    Ok(())
}

pub fn allowed_file(role: &str, path: &str) -> bool {
    let parts: Vec<_> = path.split('/').collect();
    match parts.as_slice() {
        [name] => {
            matches!(
                *name,
                "qt.conf"
                    | "THIRD_PARTY_NOTICES.md"
                    | "使用说明.md"
                    | "Qt6Core.dll"
                    | "Qt6Gui.dll"
                    | "Qt6Network.dll"
                    | "Qt6WebSockets.dll"
                    | "Qt6Widgets.dll"
                    | "Qt6Svg.dll"
                    | "concrt140.dll"
                    | "msvcp140.dll"
                    | "msvcp140_1.dll"
                    | "msvcp140_2.dll"
                    | "msvcp140_atomic_wait.dll"
                    | "msvcp140_codecvt_ids.dll"
                    | "vccorlib140.dll"
                    | "vcruntime140.dll"
                    | "vcruntime140_1.dll"
                    | "vcruntime140_threads.dll"
            ) || (role == "client" && matches!(*name, "wechat-lite.exe" | "wechat-link.exe"))
                || (role == "server" && matches!(*name, "wechat-server.exe" | "wechat-relay.exe"))
        }
        [folder, name]
            if matches!(
                *folder,
                "generic" | "imageformats" | "networkinformation" | "platforms" | "styles" | "tls"
            ) =>
        {
            name.ends_with(".dll")
        }
        ["licenses", name] => {
            name.ends_with(".txt")
                || name.ends_with(".md")
                || name.ends_with(".h")
                || *name == "FFmpeg-COPYING.GPLv3"
        }
        ["thirdparty-sources", name] if role == "server" => {
            name.ends_with(".tar.xz")
                || name.ends_with(".tar.gz")
                || name.ends_with(".txt")
                || *name == "FFmpeg-COPYING.GPLv3"
        }
        ["adapter", name] if role == "server" => matches!(
            *name,
            "wechat-adapter.exe" | "WeChatActions.dll" | "WeChatQuote.dll" | "ffmpeg.exe"
        ),
        _ => false,
    }
}

pub fn validate_manifest(manifest: &Manifest) -> Result<()> {
    let entry = match manifest.role.as_str() {
        "client" => "wechat-lite.exe",
        "server" => "wechat-server.exe",
        _ => bail!("unknown package role"),
    };
    ensure!(manifest.entry == entry, "wrong package entry point");
    ensure!(
        !manifest.files.is_empty() && manifest.files.len() <= 4096,
        "invalid package file count"
    );
    let mut paths = BTreeSet::new();
    let mut total = 0u64;
    for file in &manifest.files {
        validate_path(&file.path)?;
        ensure!(
            allowed_file(&manifest.role, &file.path),
            "unexpected release file: {}",
            file.path
        );
        ensure!(
            valid_hash(&file.sha256) && file.size <= MAX_PAYLOAD,
            "invalid package metadata"
        );
        ensure!(
            paths.insert(file.path.to_lowercase()),
            "duplicate package path"
        );
        total = total
            .checked_add(file.size)
            .context("package length overflow")?;
        ensure!(total <= MAX_PAYLOAD, "package too large");
    }
    ensure!(paths.contains(entry), "missing entry point");
    for path in &paths {
        let mut parent = Path::new(path).parent();
        while let Some(value) = parent.filter(|p| !p.as_os_str().is_empty()) {
            ensure!(
                !paths.contains(&value.to_string_lossy().replace('\\', "/")),
                "file/directory collision"
            );
            parent = value.parent();
        }
    }
    Ok(())
}

pub fn fingerprint(reader: &mut impl Read) -> Result<(u64, String)> {
    let mut digest = Sha256::new();
    let mut length = 0u64;
    let mut buffer = [0u8; 65536];
    loop {
        let n = reader.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        length += n as u64;
        digest.update(&buffer[..n]);
    }
    Ok((length, format!("{:x}", digest.finalize())))
}

impl Package {
    pub fn open_payload(path: &Path, expected_hash: &str) -> Result<Self> {
        let file = open_regular(path)?;
        let length = file.metadata()?.len();
        Self::from_range(file, 0, length, expected_hash)
    }

    pub fn from_executable(path: &Path, expected_hash: &str) -> Result<Self> {
        let mut file = open_regular(path)?;
        let length = file.metadata()?.len();
        ensure!(length >= 24, "portable EXE has no payload footer");
        let end = executable_data_end(&mut file, length)?;
        for padding in 0..=if end == length { 0 } else { 7 } {
            if end < 24 + padding {
                break;
            }
            let position = end - padding - 24;
            let footer = read_at::<24>(&mut file, position)?;
            if &footer[..8] != FOOTER {
                continue;
            }
            let offset = u64::from_le_bytes(footer[8..16].try_into().unwrap());
            let size = u64::from_le_bytes(footer[16..24].try_into().unwrap());
            ensure!(
                offset > 0 && offset.checked_add(size) == Some(position),
                "invalid portable EXE footer"
            );
            if padding > 0 {
                file.seek(SeekFrom::Start(end - padding))?;
                let mut bytes = [0u8; 7];
                file.read_exact(&mut bytes[..padding as usize])?;
                ensure!(
                    bytes[..padding as usize].iter().all(|b| *b == 0),
                    "invalid Authenticode alignment"
                );
            }
            return Self::from_range(file, offset, size, expected_hash);
        }
        bail!("portable EXE has no payload footer")
    }

    fn from_range(mut file: File, offset: u64, size: u64, expected_hash: &str) -> Result<Self> {
        ensure!(
            size >= 16 && size <= MAX_PAYLOAD && valid_hash(expected_hash),
            "invalid package size/hash"
        );
        file.seek(SeekFrom::Start(offset))?;
        let (read, actual_hash) = fingerprint(&mut (&mut file).take(size))?;
        ensure!(
            read == size && actual_hash == expected_hash,
            "complete package SHA256 mismatch"
        );
        file.seek(SeekFrom::Start(offset))?;
        let mut header = [0u8; 16];
        file.read_exact(&mut header)?;
        ensure!(&header[..8] == MAGIC, "unsupported portable package format");
        let length = u64::from_le_bytes(header[8..16].try_into().unwrap());
        ensure!(
            length > 0 && length <= 4 * 1024 * 1024 && length + 16 <= size,
            "invalid package manifest length"
        );
        let mut bytes = vec![0u8; length as usize];
        file.read_exact(&mut bytes)?;
        let manifest: Manifest = serde_json::from_slice(&bytes)?;
        validate_manifest(&manifest)?;
        let total: u64 = manifest.files.iter().map(|f| f.size).sum();
        ensure!(
            total == size - length - 16,
            "truncated or trailing package data"
        );
        Ok(Self {
            manifest,
            hash: expected_hash.to_owned(),
            source: Mutex::new(file),
            body_offset: offset + 16 + length,
        })
    }
}

pub fn reject_link(path: &Path) -> Result<fs::Metadata> {
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        !metadata.file_type().is_symlink(),
        "links are not allowed: {}",
        path.display()
    );
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        ensure!(
            metadata.file_attributes() & 0x400 == 0,
            "reparse points are not allowed: {}",
            path.display()
        );
    }
    Ok(metadata)
}

pub fn check_ancestors(path: &Path) -> Result<()> {
    ensure!(path.is_absolute(), "cache/source path must be absolute");
    for ancestor in path.ancestors() {
        if ancestor.exists() {
            reject_link(ancestor)?;
        }
    }
    Ok(())
}

pub fn open_regular(path: &Path) -> Result<File> {
    ensure!(reject_link(path)?.is_file(), "not a regular file");
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.custom_flags(windows_sys::Win32::Storage::FileSystem::FILE_FLAG_OPEN_REPARSE_POINT);
    }
    let file = options.open(path)?;
    platform::regular_file(&file)?;
    Ok(file)
}

fn private_subdirs(root: &Path, relative: &Path) -> Result<PathBuf> {
    let mut current = root.to_owned();
    for component in relative.components() {
        let Component::Normal(name) = component else {
            bail!("invalid cache directory");
        };
        current.push(name);
        platform::private_directory(&current)?;
    }
    Ok(current)
}

fn hash_file(path: &Path, expected: &Entry) -> Result<()> {
    let metadata = reject_link(path)?;
    ensure!(
        metadata.is_file() && metadata.len() == expected.size,
        "cached file size mismatch"
    );
    let mut file = open_regular(path)?;
    ensure!(
        file.metadata()?.len() == expected.size,
        "cached file changed"
    );
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let length = file.read(&mut buffer)?;
        if length == 0 {
            break;
        }
        digest.update(&buffer[..length]);
    }
    ensure!(
        format!("{:x}", digest.finalize()) == expected.sha256,
        "cached file SHA256 mismatch"
    );
    Ok(())
}

fn verify_cache(directory: &Path, package: &Package) -> Result<()> {
    ensure!(reject_link(directory)?.is_dir(), "invalid cache directory");
    let marker = directory.join(READY);
    let mut marker = open_regular(&marker)?;
    ensure!(marker.metadata()?.len() == 64, "incomplete cache");
    let mut marker_bytes = [0u8; 64];
    marker.read_exact(&mut marker_bytes)?;
    ensure!(
        marker_bytes == package.hash.as_bytes(),
        "wrong cache version"
    );
    let mut expected = BTreeSet::from([READY.to_owned()]);
    for entry in &package.manifest.files {
        expected.insert(entry.path.clone());
        let mut parent = Path::new(&entry.path).parent();
        while let Some(value) = parent.filter(|p| !p.as_os_str().is_empty()) {
            expected.insert(value.to_string_lossy().replace('\\', "/"));
            parent = value.parent();
        }
        let path = directory.join(&entry.path);
        check_ancestors(&path)?;
        hash_file(&path, entry).with_context(|| format!("invalid cached file: {}", entry.path))?;
    }
    fn check_tree(root: &Path, directory: &Path, expected: &BTreeSet<String>) -> Result<()> {
        for entry in fs::read_dir(directory)? {
            let path = entry?.path();
            let metadata = reject_link(&path)?;
            let relative = path
                .strip_prefix(root)?
                .to_str()
                .context("invalid cached filename")?
                .replace('\\', "/");
            ensure!(
                expected.contains(&relative),
                "unexpected cached file or directory"
            );
            if metadata.is_dir() {
                check_tree(root, &path, expected)?;
            }
        }
        Ok(())
    }
    check_tree(directory, directory, &expected)?;
    Ok(())
}

fn nonce() -> String {
    format!(
        "{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    )
}

pub fn install(root: &Path, package: &Package) -> Result<PathBuf> {
    check_ancestors(root)?;
    platform::private_directory(root)?;
    let role = private_subdirs(root, Path::new(&package.manifest.role))?;
    let lock_path = role.join(format!("{}.lock", package.hash));
    let mut options = OpenOptions::new();
    options.create(true).truncate(false).read(true).write(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.custom_flags(windows_sys::Win32::Storage::FileSystem::FILE_FLAG_OPEN_REPARSE_POINT);
    }
    let lock = options.open(&lock_path)?;
    ensure!(reject_link(&lock_path)?.is_file(), "invalid cache lock");
    platform::regular_file(&lock)?;
    lock.lock().context("cannot lock portable cache")?;
    let directory = role.join(&package.hash);
    if directory.exists() {
        reject_link(&directory)?;
        if verify_cache(&directory, package).is_ok() {
            return Ok(directory);
        }
        // Preserve damaged caches and any user-created files; never recursively delete a version.
        fs::rename(
            &directory,
            role.join(format!("{}-preserved-{}", package.hash, nonce())),
        )
        .context("cannot preserve damaged cache; close this version and retry")?;
    }
    let stage = role.join(format!("{}-partial-{}", package.hash, nonce()));
    platform::private_directory(&stage)?;
    let mut source = package
        .source
        .lock()
        .map_err(|_| anyhow::anyhow!("package reader lock failed"))?;
    source.seek(SeekFrom::Start(package.body_offset))?;
    for entry in &package.manifest.files {
        let relative = Path::new(&entry.path);
        if let Some(parent) = relative.parent().filter(|p| !p.as_os_str().is_empty()) {
            private_subdirs(&stage, parent)?;
        }
        let path = stage.join(relative);
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)?;
        let (length, digest) = copy_verified(&mut (&mut *source).take(entry.size), &mut file)?;
        ensure!(
            length == entry.size && digest == entry.sha256,
            "package file changed: {}",
            entry.path
        );
        file.sync_all()?;
    }
    let mut marker = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(stage.join(READY))?;
    marker.write_all(package.hash.as_bytes())?;
    marker.sync_all()?;
    drop(marker);
    verify_cache(&stage, package)?;
    fs::rename(&stage, &directory).context("cannot atomically publish portable cache")?;
    Ok(directory)
}

pub fn copy_verified(reader: &mut impl Read, writer: &mut impl Write) -> Result<(u64, String)> {
    let mut digest = Sha256::new();
    let mut length = 0u64;
    let mut buffer = [0u8; 65536];
    loop {
        let n = reader.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        writer.write_all(&buffer[..n])?;
        digest.update(&buffer[..n]);
        length += n as u64;
    }
    Ok((length, format!("{:x}", digest.finalize())))
}

fn read_at<const N: usize>(file: &mut File, position: u64) -> Result<[u8; N]> {
    file.seek(SeekFrom::Start(position))?;
    let mut bytes = [0u8; N];
    file.read_exact(&mut bytes)?;
    Ok(bytes)
}

fn pe_headers(file: &mut File) -> Result<(u64, u64, u16, u16)> {
    ensure!(
        read_at::<2>(file, 0)? == *b"MZ",
        "launcher is not a PE image"
    );
    let pe = u32::from_le_bytes(read_at::<4>(file, 60)?) as u64;
    ensure!(read_at::<4>(file, pe)? == *b"PE\0\0", "invalid PE image");
    ensure!(
        u16::from_le_bytes(read_at::<2>(file, pe + 4)?) == 0x8664,
        "launcher must be Windows x64"
    );
    let count = u16::from_le_bytes(read_at::<2>(file, pe + 6)?);
    let optional_size = u16::from_le_bytes(read_at::<2>(file, pe + 20)?);
    ensure!(
        (1..=96).contains(&count) && optional_size >= 160,
        "invalid PE headers"
    );
    let optional = pe + 24;
    ensure!(
        u16::from_le_bytes(read_at::<2>(file, optional)?) == 0x20b,
        "launcher must be PE32+"
    );
    Ok((
        optional,
        optional + optional_size as u64,
        count,
        optional_size,
    ))
}

fn executable_data_end(file: &mut File, length: u64) -> Result<u64> {
    // Non-PE containers are useful in parser fixtures; production packaging validates the PE separately.
    if read_at::<2>(file, 0)? != *b"MZ" {
        return Ok(length);
    }
    let (optional, _, _, _) = pe_headers(file)?;
    let security = read_at::<8>(file, optional + 144)?;
    let offset = u32::from_le_bytes(security[..4].try_into().unwrap()) as u64;
    let size = u32::from_le_bytes(security[4..].try_into().unwrap()) as u64;
    if offset == 0 && size == 0 {
        return Ok(length);
    }
    ensure!(
        offset > 0 && size >= 8 && offset.checked_add(size) == Some(length),
        "invalid PE certificate table"
    );
    Ok(offset)
}

pub fn verify_launcher_pe(path: &Path) -> Result<Vec<String>> {
    let mut file = open_regular(path)?;
    let (optional, section_table, count, _) = pe_headers(&mut file)?;
    ensure!(
        u16::from_le_bytes(read_at::<2>(&mut file, optional + 68)?) == 2,
        "launcher must use Windows GUI subsystem"
    );
    let mut sections = Vec::new();
    for i in 0..count {
        let section = read_at::<40>(&mut file, section_table + i as u64 * 40)?;
        let get =
            |offset| u32::from_le_bytes(section[offset..offset + 4].try_into().unwrap()) as u64;
        sections.push((get(12), get(16), get(20)));
    }
    let to_file = |rva: u32| -> Result<u64> {
        let rva = rva as u64;
        for &(virtual_start, raw_size, raw_start) in &sections {
            if rva >= virtual_start && rva - virtual_start < raw_size {
                return Ok(raw_start + rva - virtual_start);
            }
        }
        bail!("invalid PE import address")
    };
    let imports_rva = u32::from_le_bytes(read_at::<4>(&mut file, optional + 120)?);
    let imports = to_file(imports_rva)?;
    let mut names = Vec::new();
    for index in 0..256 {
        let descriptor = read_at::<20>(&mut file, imports + index * 20)?;
        if descriptor.iter().all(|b| *b == 0) {
            return Ok(names);
        }
        let name_rva = u32::from_le_bytes(descriptor[12..16].try_into().unwrap());
        let name_offset = to_file(name_rva)?;
        file.seek(SeekFrom::Start(name_offset))?;
        let mut name = Vec::new();
        for _ in 0..256 {
            let mut byte = [0];
            file.read_exact(&mut byte)?;
            if byte[0] == 0 {
                break;
            }
            name.push(byte[0]);
        }
        let name = String::from_utf8(name)?.to_ascii_lowercase();
        let system = matches!(
            name.as_str(),
            "advapi32.dll"
                | "combase.dll"
                | "bcrypt.dll"
                | "bcryptprimitives.dll"
                | "crypt32.dll"
                | "kernel32.dll"
                | "kernelbase.dll"
                | "ntdll.dll"
                | "ole32.dll"
                | "oleaut32.dll"
                | "shell32.dll"
                | "user32.dll"
                | "userenv.dll"
                | "ws2_32.dll"
                | "gdi32.dll"
                | "shlwapi.dll"
                | "rpcrt4.dll"
                | "sechost.dll"
                | "version.dll"
                | "normaliz.dll"
        ) || (name.starts_with("api-ms-win-") && !name.starts_with("api-ms-win-crt-"));
        ensure!(
            system,
            "launcher imports a non-system DLL (static CRT required): {name}"
        );
        names.push(name);
    }
    bail!("too many PE imports")
}
