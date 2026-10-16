use anyhow::{Result, ensure};
use std::{collections::BTreeSet, ffi::c_void, mem::size_of, path::PathBuf};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE},
    System::{
        Diagnostics::{
            Debug::ReadProcessMemory,
            ToolHelp::{
                CreateToolhelp32Snapshot, MODULEENTRY32W, Module32FirstW, Module32NextW,
                PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPMODULE,
                TH32CS_SNAPMODULE32, TH32CS_SNAPPROCESS,
            },
        },
        Memory::{MEM_COMMIT, MEMORY_BASIC_INFORMATION, PAGE_GUARD, VirtualQueryEx},
        Registry::{
            HKEY_CURRENT_USER, KEY_READ, REG_EXPAND_SZ, REG_SZ, RegCloseKey, RegEnumValueW,
            RegOpenKeyExW,
        },
        Threading::{OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ},
    },
};

struct Handle(HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain([0]).collect()
}
fn from_wide(value: &[u16]) -> String {
    String::from_utf16_lossy(&value[..value.iter().position(|v| *v == 0).unwrap_or(value.len())])
}
fn pointer(value: u64) -> bool {
    (0x10000..0x800000000000).contains(&value)
}
fn u64_at(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        data.get(offset..offset + 8)?.try_into().ok()?,
    ))
}

pub fn pids() -> Result<Vec<u32>> {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    ensure!(snapshot != INVALID_HANDLE_VALUE, "无法枚举本机微信进程");
    let snapshot = Handle(snapshot);
    let mut entry: PROCESSENTRY32W = unsafe { std::mem::zeroed() };
    entry.dwSize = size_of::<PROCESSENTRY32W>() as u32;
    let mut ok = unsafe { Process32FirstW(snapshot.0, &mut entry) };
    let mut result = Vec::new();
    while ok != 0 {
        if from_wide(&entry.szExeFile).eq_ignore_ascii_case("Weixin.exe") {
            result.push(entry.th32ProcessID);
        }
        entry.dwSize = size_of::<PROCESSENTRY32W>() as u32;
        ok = unsafe { Process32NextW(snapshot.0, &mut entry) };
    }
    Ok(result)
}

struct Process {
    handle: Handle,
}
impl Process {
    fn open(pid: u32) -> Result<Self> {
        let handle = unsafe { OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, 0, pid) };
        ensure!(
            !handle.is_null(),
            "无法只读访问本机微信进程，请使用相同用户运行"
        );
        Ok(Self {
            handle: Handle(handle),
        })
    }
    fn read(&self, address: usize, length: usize) -> Option<Vec<u8>> {
        if length == 0 || length > 1024 * 1024 + 512 || !pointer(address as u64) {
            return None;
        }
        let mut bytes = vec![0; length];
        let mut count = 0;
        unsafe {
            ReadProcessMemory(
                self.handle.0,
                address as *const c_void,
                bytes.as_mut_ptr().cast(),
                length,
                &mut count,
            );
        }
        if count == 0 {
            return None;
        }
        bytes.truncate(count);
        Some(bytes)
    }
    fn number(&self, address: usize) -> Option<u64> {
        let data = self.read(address, 8)?;
        u64_at(&data, 0)
    }
    fn string(&self, address: usize) -> Option<Vec<u8>> {
        let object = self.read(address, 32)?;
        let length = u64_at(&object, 16)? as usize;
        let capacity = u64_at(&object, 24)?;
        if length == 0 || length > 1024 {
            return None;
        }
        let data = if capacity <= 15 {
            self.read(address, length)?
        } else {
            self.read(u64_at(&object, 0)? as usize, length)?
        };
        (data.len() == length).then_some(data)
    }
    fn scan(&self, start: usize, end: usize, mut visit: impl FnMut(usize, &[u8]) -> bool) -> bool {
        let mut address = start;
        while address < end {
            let mut region: MEMORY_BASIC_INFORMATION = unsafe { std::mem::zeroed() };
            if unsafe {
                VirtualQueryEx(
                    self.handle.0,
                    address as *const c_void,
                    &mut region,
                    size_of::<MEMORY_BASIC_INFORMATION>(),
                )
            } == 0
            {
                break;
            }
            let base = region.BaseAddress as usize;
            let next = base.saturating_add(region.RegionSize);
            if next <= address {
                break;
            }
            if region.State == MEM_COMMIT
                && region.Protect & PAGE_GUARD == 0
                && region.Protect & 0xee != 0
            {
                let mut position = base.max(start);
                let last = next.min(end);
                let mut overlap = Vec::new();
                while position < last {
                    let size = (last - position).min(1024 * 1024);
                    if let Some(bytes) = self.read(position, size) {
                        let offset = position - overlap.len();
                        overlap.extend(bytes);
                        if visit(offset, &overlap) {
                            return true;
                        }
                        if overlap.len() > 512 {
                            overlap.drain(..overlap.len() - 512);
                        }
                    } else {
                        overlap.clear();
                    }
                    position += size;
                }
            }
            address = next;
        }
        false
    }
    fn find(&self, patterns: &[Vec<u8>], range: Option<(usize, usize)>) -> Vec<usize> {
        let mut hits = BTreeSet::new();
        let (start, end) = range.unwrap_or((0, 0x800000000000));
        self.scan(start, end, |base, bytes| {
            for pattern in patterns {
                let mut offset = 0;
                while let Some(found) = memchr::memmem::find(&bytes[offset..], pattern) {
                    hits.insert(base + offset + found);
                    offset += found + 1;
                    if hits.len() > 4096 {
                        return true;
                    }
                }
            }
            false
        });
        hits.into_iter().collect()
    }
}

pub fn scan_memory(mut visitor: impl FnMut(u32, usize, &[u8]) -> bool) -> Result<()> {
    let processes = pids()?;
    ensure!(!processes.is_empty(), "未找到已运行的本机微信");
    for pid in processes {
        let process = Process::open(pid)?;
        if process.scan(0, 0x800000000000, |address, bytes| {
            visitor(pid, address, bytes)
        }) {
            break;
        }
    }
    Ok(())
}

pub struct Secrets {
    pub candidates: Vec<Vec<u8>>,
    pub configs: Vec<([u8; 32], u32, String)>,
}
pub fn extract() -> Result<Secrets> {
    let pids = pids()?;
    ensure!(!pids.is_empty(), "请先登录本机 Windows 微信");
    let mut candidates = BTreeSet::new();
    let mut configs = Vec::new();
    for pid in pids {
        let Ok(process) = Process::open(pid) else {
            continue;
        };
        const NAME: &[u8] = b"com.Tencent.WCDB.Config.Cipher";
        let names = process.find(&[NAME.to_vec()], None);
        let patterns = names
            .iter()
            .map(|address| {
                [
                    (*address as u64).to_le_bytes().as_slice(),
                    (NAME.len() as u64).to_le_bytes().as_slice(),
                ]
                .concat()
            })
            .collect::<Vec<_>>();
        if !patterns.is_empty() {
            for address in process.find(&patterns, None) {
                let Some(node) = address.checked_sub(16).and_then(|a| process.read(a, 0x50)) else {
                    continue;
                };
                if !names.contains(&(u64_at(&node, 0x10).unwrap_or(0) as usize))
                    || u64_at(&node, 0x18) != Some(NAME.len() as u64)
                {
                    continue;
                }
                let Some(config) = u64_at(&node, 0x28).filter(|v| pointer(*v)) else {
                    continue;
                };
                let Some(object) = process.read(config as usize + 0x88, 0x28) else {
                    continue;
                };
                let (Some(data), Some(length)) = (u64_at(&object, 8), u64_at(&object, 16)) else {
                    continue;
                };
                if !pointer(data) || !(1..=1024).contains(&length) {
                    continue;
                }
                let Some(mut blob) = process.read(data as usize, length as usize) else {
                    continue;
                };
                let mask =
                    hex::decode("d2c7442458020000004889442450488b450048844c2448488944254048584c24")
                        .expect("static mask");
                for (index, value) in blob.iter_mut().enumerate() {
                    *value ^= mask[index % 32];
                }
                collect_literals(&blob, &mut candidates);
            }
        }
        if let Some(module) = module(pid) {
            configs.extend(configs_from_module(&process, module));
        }
    }
    ensure!(
        !candidates.is_empty() || !configs.is_empty(),
        "无法读取当前微信数据库密钥，此微信版本可能尚未支持"
    );
    Ok(Secrets {
        candidates: candidates.into_iter().collect(),
        configs,
    })
}

fn collect_literals(blob: &[u8], result: &mut BTreeSet<Vec<u8>>) {
    for start in 0..blob.len().saturating_sub(3) {
        if !matches!(blob[start], b'x' | b'X') || blob[start + 1] != b'\'' {
            continue;
        }
        let run = &blob[start + 2..];
        let length = run.iter().take_while(|v| v.is_ascii_hexdigit()).count();
        if !(64..=192).contains(&length) || run.get(length) != Some(&b'\'') {
            continue;
        }
        let mut positions = BTreeSet::from([0]);
        if length > 96 {
            positions.extend((0..=length - 64).step_by(32));
            positions.insert(length - 64);
        }
        for offset in positions {
            let Ok(key) = hex::decode(&run[offset..offset + 64]) else {
                continue;
            };
            if key.iter().collect::<BTreeSet<_>>().len() < 15 {
                continue;
            }
            result.insert(key.clone());
            if offset + 96 <= length {
                if let Ok(full) = hex::decode(&run[offset..offset + 96]) {
                    result.insert(full);
                }
            }
        }
    }
}

fn module(pid: u32) -> Option<(usize, usize, PathBuf)> {
    let snapshot =
        unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid) };
    if snapshot == INVALID_HANDLE_VALUE {
        return None;
    }
    let snapshot = Handle(snapshot);
    let mut entry: MODULEENTRY32W = unsafe { std::mem::zeroed() };
    entry.dwSize = size_of::<MODULEENTRY32W>() as u32;
    let mut ok = unsafe { Module32FirstW(snapshot.0, &mut entry) };
    while ok != 0 {
        if from_wide(&entry.szModule).eq_ignore_ascii_case("weixin.dll") {
            return Some((
                entry.modBaseAddr as usize,
                entry.modBaseSize as usize,
                from_wide(&entry.szExePath).into(),
            ));
        }
        entry.dwSize = size_of::<MODULEENTRY32W>() as u32;
        ok = unsafe { Module32NextW(snapshot.0, &mut entry) };
    }
    None
}

fn material(path: &std::path::Path) -> Option<[u8; 32]> {
    use std::io::Read;
    let pattern=hex::decode("83ec404889d64889cb0f57c00f1142100f11024c8bb1c80200004883b9d0020000107209488b9bb8020000eb074881c3b80200004d85f60f880a0200004983fe10736d4c89761048c746180f0000000f10030f110648b8").ok()?;
    let mut file = std::fs::File::open(path).ok()?;
    let mut block = vec![0; 1024 * 1024];
    let mut overlap = Vec::new();
    loop {
        let count = file.read(&mut block).ok()?;
        if count == 0 {
            break;
        }
        overlap.extend_from_slice(&block[..count]);
        if let Some(found) = memchr::memmem::find(&overlap, &pattern) {
            let data = &overlap[found + pattern.len()..];
            if data.len() < 53 {
                continue;
            }
            let mut result = [0; 32];
            let verify = [
                &b"\x48\x89\x44\x24\x20\x48\xb8"[..],
                &b"\x48\x89\x44\x24\x28\x48\xb8"[..],
                &b"\x48\x89\x44\x24\x30\x48\xb8"[..],
            ];
            for index in 0..4 {
                let offset = index * 15;
                result[index * 8..index * 8 + 8].copy_from_slice(data.get(offset..offset + 8)?);
                if index < 3 && data.get(offset + 8..offset + 15)? != verify[index] {
                    return None;
                }
            }
            return Some(result);
        }
        if overlap.len() > 256 {
            overlap.drain(..overlap.len() - 256);
        }
    }
    None
}

fn configs_from_module(
    process: &Process,
    (base, size, path): (usize, usize, PathBuf),
) -> Vec<([u8; 32], u32, String)> {
    let Some(material) = material(&path) else {
        return Vec::new();
    };
    let mut result = Vec::new();
    for landmark in process.find(
        &[b"global_config".to_vec()],
        Some((base, base.saturating_add(size))),
    ) {
        let Some(layout) = process.read(landmark + 16, 16) else {
            continue;
        };
        if u64_at(&layout, 0) != Some(13) || u64_at(&layout, 8) != Some(15) {
            continue;
        }
        for back in [0x138, 0x130] {
            let Some(root) = landmark
                .checked_add(16)
                .and_then(|v| v.checked_sub(back))
                .and_then(|a| process.number(a))
                .filter(|v| pointer(*v))
            else {
                continue;
            };
            let Some(config) = process.number(root as usize + 0x68).filter(|v| pointer(*v)) else {
                continue;
            };
            let Some(cipher) = process
                .string(config as usize + 0x2b8)
                .filter(|v| v.len() == 32)
            else {
                continue;
            };
            let Some(wxid) = process
                .string(config as usize + 0x48)
                .and_then(|v| String::from_utf8(v).ok())
                .filter(|v| !v.is_empty() && v.len() < 128)
            else {
                continue;
            };
            let Some(dword) = process
                .read(config as usize + 0x40, 4)
                .and_then(|v| v.try_into().ok())
                .map(u32::from_le_bytes)
            else {
                continue;
            };
            let mut master = [0; 32];
            for index in 0..32 {
                master[index] = cipher[index] ^ material[index];
            }
            result.push((master, dword, wxid));
        }
    }
    result
}

pub fn registry_paths() -> Vec<String> {
    let mut result = Vec::new();
    for path in [
        "Software\\Tencent\\xwechat",
        "Software\\Tencent\\xwechat\\config",
        "Software\\Tencent\\WeChat",
    ] {
        let mut key = std::ptr::null_mut();
        if unsafe {
            RegOpenKeyExW(
                HKEY_CURRENT_USER,
                wide(path).as_ptr(),
                0,
                KEY_READ,
                &mut key,
            )
        } != 0
        {
            continue;
        }
        for index in 0..1024 {
            let mut name = [0u16; 512];
            let mut length = name.len() as u32;
            let mut data = [0u8; 8192];
            let mut size = data.len() as u32;
            let mut kind = 0;
            if unsafe {
                RegEnumValueW(
                    key,
                    index,
                    name.as_mut_ptr(),
                    &mut length,
                    std::ptr::null_mut(),
                    &mut kind,
                    data.as_mut_ptr(),
                    &mut size,
                )
            } != 0
            {
                break;
            }
            let name = from_wide(&name).to_ascii_lowercase();
            if matches!(kind, REG_SZ | REG_EXPAND_SZ)
                && ["path", "dir", "save"]
                    .iter()
                    .any(|part| name.contains(part))
            {
                let text = data[..size as usize]
                    .chunks_exact(2)
                    .map(|p| u16::from_le_bytes([p[0], p[1]]))
                    .collect::<Vec<_>>();
                result.push(from_wide(&text));
            }
        }
        unsafe {
            RegCloseKey(key);
        }
    }
    result
}
