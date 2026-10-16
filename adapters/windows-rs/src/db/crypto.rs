use aes::Aes256;
use anyhow::{Context, Result, bail, ensure};
use cbc::cipher::{BlockDecryptMut, KeyIvInit, block_padding::NoPadding};
use hmac::{Hmac, Mac};
use rusqlite::{Connection, ffi, serialize::OwnedData};
use sha2::Sha512;
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::Path,
    ptr::NonNull,
};

pub const PAGE: usize = 4096;
const RESERVE: usize = 80;

#[derive(Clone)]
pub struct Key {
    pub data: [u8; 32],
    pub explicit_salt: Option<[u8; 16]>,
}

impl Key {
    pub fn checked(bytes: &[u8], page: &[u8]) -> Option<Self> {
        if !matches!(bytes.len(), 32 | 48) || page.len() != PAGE {
            return None;
        }
        let key = Self {
            data: bytes[..32].try_into().ok()?,
            explicit_salt: if bytes.len() == 48 {
                Some(bytes[32..].try_into().ok()?)
            } else {
                None
            },
        };
        let mac_key = key.mac_key(page);
        key.verify(page, 1, &mac_key).then_some(key)
    }
    fn mac_key(&self, page: &[u8]) -> [u8; 32] {
        let salt = self
            .explicit_salt
            .as_ref()
            .map(|v| v.as_slice())
            .unwrap_or(&page[..16]);
        let salt: Vec<_> = salt.iter().map(|b| b ^ 0x3a).collect();
        let mut key = [0; 32];
        pbkdf2::pbkdf2_hmac::<Sha512>(&self.data, &salt, 2, &mut key);
        key
    }
    fn verify(&self, page: &[u8], number: u32, mac_key: &[u8; 32]) -> bool {
        if page.len() != PAGE {
            return false;
        }
        let mut mac = <Hmac<Sha512> as Mac>::new_from_slice(mac_key).expect("fixed HMAC key");
        mac.update(&page[if number == 1 { 16 } else { 0 }..PAGE - 64]);
        mac.update(&number.to_le_bytes());
        mac.verify_slice(&page[PAGE - 64..]).is_ok()
    }
    fn decrypt(
        &self,
        page: &[u8],
        number: u32,
        mac_key: &[u8; 32],
        target: &mut [u8],
    ) -> Result<()> {
        ensure!(
            number > 0 && target.len() == PAGE && self.verify(page, number, mac_key),
            "微信数据库页校验失败，本轮未推进游标"
        );
        let start = if number == 1 { 16 } else { 0 };
        target.fill(0);
        target[start..PAGE - RESERVE].copy_from_slice(&page[start..PAGE - RESERVE]);
        cbc::Decryptor::<Aes256>::new((&self.data).into(), page[PAGE - RESERVE..PAGE - 64].into())
            .decrypt_padded_mut::<NoPadding>(&mut target[start..PAGE - RESERVE])
            .map_err(|_| anyhow::anyhow!("微信数据库页解密失败"))?;
        if number == 1 {
            target[..16].copy_from_slice(if self.explicit_salt.is_some() {
                &page[..16]
            } else {
                b"SQLite format 3\0"
            });
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Hash, PartialEq, Eq)]
pub struct Signature(pub u64, pub u64, pub (u64, u64));
pub type SnapshotVersion = (Signature, Option<Signature>);

impl Signature {
    pub fn token(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut hash = std::collections::hash_map::DefaultHasher::new();
        self.hash(&mut hash);
        hash.finish()
    }
}

fn file_signature(file: &File) -> Result<Signature> {
    let info = file.metadata()?;
    #[cfg(windows)]
    let identity = {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::Storage::FileSystem::{
            BY_HANDLE_FILE_INFORMATION, GetFileInformationByHandle,
        };
        let mut identity: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
        ensure!(
            unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut identity) } != 0,
            "微信数据库文件身份不可读取"
        );
        (
            identity.dwVolumeSerialNumber as u64,
            (identity.nFileIndexHigh as u64) << 32 | identity.nFileIndexLow as u64,
        )
    };
    #[cfg(unix)]
    let identity = {
        use std::os::unix::fs::MetadataExt;
        (info.dev(), info.ino())
    };
    #[cfg(not(any(windows, unix)))]
    let identity = (0, 0);
    Ok(Signature(
        info.modified()?
            .duration_since(std::time::UNIX_EPOCH)?
            .as_nanos()
            .try_into()
            .unwrap_or(u64::MAX),
        info.len(),
        identity,
    ))
}

pub fn signature(path: &Path) -> Result<Option<Signature>> {
    match File::open(path) {
        Ok(file) => file_signature(&file).map(Some),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}

pub fn wal_path(source: &Path) -> Result<std::path::PathBuf> {
    Ok(source.with_file_name(format!(
        "{}-wal",
        source
            .file_name()
            .context("数据库文件名无效")?
            .to_string_lossy()
    )))
}

pub fn version(source: &Path) -> Result<Option<SnapshotVersion>> {
    let Some(main) = signature(source)? else {
        return Ok(None);
    };
    Ok(Some((main, signature(&wal_path(source)?)?)))
}

fn be(data: &[u8]) -> u32 {
    u32::from_be_bytes(data.try_into().expect("four bytes"))
}
fn checksum(data: &[u8], little: bool, mut previous: (u32, u32)) -> (u32, u32) {
    for pair in data.chunks_exact(8) {
        let word = |bytes: &[u8]| {
            if little {
                u32::from_le_bytes(bytes.try_into().unwrap())
            } else {
                be(bytes)
            }
        };
        previous.0 = previous
            .0
            .wrapping_add(word(&pair[..4]))
            .wrapping_add(previous.1);
        previous.1 = previous
            .1
            .wrapping_add(word(&pair[4..]))
            .wrapping_add(previous.0);
    }
    previous
}

#[derive(Default, Debug, PartialEq, Eq)]
struct Commit {
    frames: u64,
    pages: Option<u32>,
}

fn committed(wal: &mut File) -> Result<Commit> {
    let mut header = [0; 32];
    match wal.read_exact(&mut header) {
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(Commit::default()),
        result => result?,
    }
    let magic = be(&header[..4]);
    ensure!(
        matches!(magic, 0x377f0682 | 0x377f0683)
            && be(&header[4..8]) == 3007000
            && be(&header[8..12]) == PAGE as u32,
        "微信 WAL 头无效，本轮未推进游标"
    );
    let little = magic == 0x377f0682;
    let mut running = checksum(&header[..24], little, (0, 0));
    ensure!(
        running == (be(&header[24..28]), be(&header[28..32])),
        "微信 WAL 头校验失败"
    );
    let mut result = Commit::default();
    let mut frame = [0; 24 + PAGE];
    let mut count = 0;
    loop {
        match wal.read_exact(&mut frame) {
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            value => value?,
        }
        if be(&frame[..4]) == 0 || frame[8..16] != header[16..24] {
            break;
        }
        running = checksum(&frame[24..], little, checksum(&frame[..8], little, running));
        if running != (be(&frame[16..20]), be(&frame[20..24])) {
            break;
        }
        count += 1;
        if be(&frame[4..8]) > 0 {
            result = Commit {
                frames: count,
                pages: Some(be(&frame[4..8])),
            };
        }
    }
    Ok(result)
}

struct Buffer {
    pointer: NonNull<u8>,
    size: usize,
}
impl Buffer {
    fn new(size: usize) -> Result<Self> {
        ensure!(size > 0, "微信数据库为空");
        let pointer = NonNull::new(unsafe { ffi::sqlite3_malloc64(size as u64) }.cast::<u8>())
            .context("内存不足，无法创建只读数据库快照")?;
        unsafe {
            std::ptr::write_bytes(pointer.as_ptr(), 0, size);
        }
        Ok(Self { pointer, size })
    }
    fn bytes(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.pointer.as_ptr(), self.size) }
    }
    fn owned(self) -> OwnedData {
        let data = unsafe { OwnedData::from_raw_nonnull(self.pointer, self.size) };
        std::mem::forget(self);
        data
    }
}
impl Drop for Buffer {
    fn drop(&mut self) {
        unsafe {
            ffi::sqlite3_free(self.pointer.as_ptr().cast());
        }
    }
}

pub fn snapshot(source: &Path, key: &Key) -> Result<(Connection, SnapshotVersion)> {
    let wal_path = wal_path(source)?;
    for _ in 0..3 {
        let before = version(source)?.context("微信数据库已移除")?;
        let size = before.0.1;
        ensure!(
            size > 0 && size % PAGE as u64 == 0,
            "微信数据库尚未写入完整页"
        );
        let mut wal = if before.1.is_some() {
            Some(File::open(&wal_path)?)
        } else {
            None
        };
        let mut input = File::open(source)?;
        if file_signature(&input)? != before.0
            || wal.as_ref().map(file_signature).transpose()? != before.1
        {
            continue;
        }
        let commit = match &mut wal {
            Some(file) => committed(file)?,
            None => Commit::default(),
        };
        let final_size = commit
            .pages
            .map(|pages| pages as u64 * PAGE as u64)
            .unwrap_or(size);
        ensure!(
            final_size <= size.saturating_add(before.1.as_ref().map(|s| s.1).unwrap_or(0)),
            "微信 WAL 数据库大小无效"
        );
        let mut memory = Buffer::new(
            final_size
                .try_into()
                .context("微信数据库超过本机地址空间")?,
        )?;
        let mut page = [0; PAGE];
        input.read_exact(&mut page)?;
        let mac_key = key.mac_key(&page);
        for number in 1..=size / PAGE as u64 {
            if number > 1 {
                input.read_exact(&mut page)?;
            }
            if number <= final_size / PAGE as u64 {
                key.decrypt(
                    &page,
                    number.try_into()?,
                    &mac_key,
                    &mut memory.bytes()[(number as usize - 1) * PAGE..number as usize * PAGE],
                )?;
            }
        }
        if let Some(wal) = &mut wal {
            wal.seek(SeekFrom::Start(32))?;
            let mut frame = [0; 24 + PAGE];
            for _ in 0..commit.frames {
                wal.read_exact(&mut frame)?;
                let number = be(&frame[..4]);
                if number as u64 <= final_size / PAGE as u64 {
                    key.decrypt(
                        &frame[24..],
                        number,
                        &mac_key,
                        &mut memory.bytes()[(number as usize - 1) * PAGE..number as usize * PAGE],
                    )?;
                }
            }
            if let Some(pages) = commit.pages {
                memory.bytes()[28..32].copy_from_slice(&pages.to_be_bytes());
            }
        }
        memory.bytes()[18..20].copy_from_slice(&[1, 1]);
        if version(source)?.as_ref() != Some(&before) {
            continue;
        }
        let mut connection = Connection::open_in_memory()?;
        connection.deserialize("main", memory.owned(), true)?;
        connection.execute_batch(
            "PRAGMA temp_store=MEMORY; PRAGMA query_only=ON; PRAGMA cache_size=-2048;",
        )?;
        let check: String = connection.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
        if version(source)?.as_ref() != Some(&before) {
            continue;
        }
        ensure!(check == "ok", "微信只读内存快照未通过完整性检查");
        return Ok((connection, before));
    }
    bail!("微信数据库正在更新，本轮未读取消息，请稍后重试")
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::io::Write;
    #[test]
    fn wal_accepts_only_checksum_valid_commits() {
        for little in [false, true] {
            let mut header = [0; 32];
            header[..4]
                .copy_from_slice(&(if little { 0x377f0682u32 } else { 0x377f0683 }).to_be_bytes());
            header[4..8].copy_from_slice(&3007000u32.to_be_bytes());
            header[8..12].copy_from_slice(&(PAGE as u32).to_be_bytes());
            header[16..24].copy_from_slice(b"12345678");
            let mut sum = checksum(&header[..24], little, (0, 0));
            header[24..28].copy_from_slice(&sum.0.to_be_bytes());
            header[28..32].copy_from_slice(&sum.1.to_be_bytes());
            let mut bytes = header.to_vec();
            for (number, commit) in [(1u32, 0u32), (2, 2), (3, 0), (4, 4)] {
                let mut frame = vec![0; 24 + PAGE];
                frame[..4].copy_from_slice(&number.to_be_bytes());
                frame[4..8].copy_from_slice(&commit.to_be_bytes());
                frame[8..16].copy_from_slice(&header[16..24]);
                sum = checksum(&frame[24..], little, checksum(&frame[..8], little, sum));
                frame[16..20].copy_from_slice(&sum.0.to_be_bytes());
                frame[20..24].copy_from_slice(&sum.1.to_be_bytes());
                if number == 4 {
                    frame[100] ^= 1;
                }
                bytes.extend(frame);
            }
            let mut file = tempfile::tempfile().unwrap();
            file.write_all(&bytes).unwrap();
            file.rewind().unwrap();
            assert_eq!(
                committed(&mut file).unwrap(),
                Commit {
                    frames: 2,
                    pages: Some(2)
                }
            );
        }
    }
    #[test]
    fn sqlcipher_key_and_every_page_are_authenticated() {
        use cbc::cipher::BlockEncryptMut;
        let bytes = [7; 32];
        let salt = [19; 16];
        let key = Key {
            data: bytes,
            explicit_salt: None,
        };
        let mut page = [0u8; PAGE];
        page[..16].copy_from_slice(&salt);
        let iv = [11; 16];
        page[PAGE - RESERVE..PAGE - 64].copy_from_slice(&iv);
        cbc::Encryptor::<Aes256>::new((&bytes).into(), (&iv).into())
            .encrypt_padded_mut::<NoPadding>(&mut page[16..PAGE - RESERVE], PAGE - RESERVE - 16)
            .unwrap();
        let mac_key = key.mac_key(&page);
        let mut mac = <Hmac<Sha512> as Mac>::new_from_slice(&mac_key).unwrap();
        mac.update(&page[16..PAGE - 64]);
        mac.update(&1u32.to_le_bytes());
        page[PAGE - 64..].copy_from_slice(&mac.finalize().into_bytes());
        assert!(Key::checked(&bytes, &page).is_some());
        assert!(Key::checked(&[8; 32], &page).is_none());
        let mut output = [0; PAGE];
        key.decrypt(&page, 1, &mac_key, &mut output).unwrap();
        assert_eq!(&output[..16], b"SQLite format 3\0");
        page[40] ^= 1;
        assert!(key.decrypt(&page, 1, &mac_key, &mut output).is_err());
    }

    pub(crate) fn encrypt(plain: &[u8], key: &Key) -> Vec<u8> {
        use cbc::cipher::BlockEncryptMut;
        let salt = [19; 16];
        let mut result = Vec::new();
        let mut initial = [0; PAGE];
        initial[..16].copy_from_slice(&salt);
        let mac_key = key.mac_key(&initial);
        for (index, source) in plain.chunks_exact(PAGE).enumerate() {
            let number = index as u32 + 1;
            let start = if number == 1 { 16 } else { 0 };
            let mut page = source.to_vec();
            if number == 1 {
                page[..16].copy_from_slice(&salt);
            }
            let iv = [(index + 3) as u8; 16];
            page[PAGE - RESERVE..PAGE - 64].copy_from_slice(&iv);
            cbc::Encryptor::<Aes256>::new((&key.data).into(), (&iv).into())
                .encrypt_padded_mut::<NoPadding>(
                    &mut page[start..PAGE - RESERVE],
                    PAGE - RESERVE - start,
                )
                .unwrap();
            let mut mac = <Hmac<Sha512> as Mac>::new_from_slice(&mac_key).unwrap();
            mac.update(&page[start..PAGE - 64]);
            mac.update(&number.to_le_bytes());
            page[PAGE - 64..].copy_from_slice(&mac.finalize().into_bytes());
            result.extend(page);
        }
        result
    }

    pub(crate) fn committed_wal(plain: &[u8], key: &Key) -> Vec<u8> {
        let encrypted = encrypt(plain, key);
        let mut header = [0u8; 32];
        header[..4].copy_from_slice(&0x377f0682u32.to_be_bytes());
        header[4..8].copy_from_slice(&3007000u32.to_be_bytes());
        header[8..12].copy_from_slice(&(PAGE as u32).to_be_bytes());
        header[16..24].copy_from_slice(b"12345678");
        let mut sum = checksum(&header[..24], true, (0, 0));
        header[24..28].copy_from_slice(&sum.0.to_be_bytes());
        header[28..32].copy_from_slice(&sum.1.to_be_bytes());
        let mut wal = header.to_vec();
        for (index, page) in encrypted.chunks_exact(PAGE).enumerate() {
            let mut frame = [0u8; 24];
            frame[..4].copy_from_slice(&(index as u32 + 1).to_be_bytes());
            frame[4..8].copy_from_slice(
                &(if (index + 1) * PAGE == encrypted.len() {
                    (encrypted.len() / PAGE) as u32
                } else {
                    0
                })
                .to_be_bytes(),
            );
            frame[8..16].copy_from_slice(&header[16..24]);
            sum = checksum(page, true, checksum(&frame[..8], true, sum));
            frame[16..20].copy_from_slice(&sum.0.to_be_bytes());
            frame[20..24].copy_from_slice(&sum.1.to_be_bytes());
            wal.extend(frame);
            wal.extend(page);
        }
        wal
    }

    #[test]
    fn read_only_sqlite_snapshot_overlays_committed_wal_and_preserves_source_files() {
        let connection = Connection::open_in_memory().unwrap();
        connection.execute_batch("PRAGMA page_size=4096;").unwrap();
        let mut reserved = RESERVE as i32;
        assert_eq!(
            unsafe {
                ffi::sqlite3_file_control(
                    connection.handle(),
                    c"main".as_ptr(),
                    ffi::SQLITE_FCNTL_RESERVE_BYTES,
                    (&mut reserved as *mut i32).cast(),
                )
            },
            ffi::SQLITE_OK
        );
        connection
            .execute_batch("CREATE TABLE fixture(value TEXT);INSERT INTO fixture VALUES('old');")
            .unwrap();
        let base = connection.serialize("main").unwrap().to_vec();
        assert_eq!(base[20], RESERVE as u8);
        connection
            .execute("UPDATE fixture SET value='committed'", [])
            .unwrap();
        let latest = connection.serialize("main").unwrap().to_vec();
        connection
            .execute("UPDATE fixture SET value='uncommitted'", [])
            .unwrap();
        let tail = connection.serialize("main").unwrap().to_vec();
        let key = Key {
            data: [7; 32],
            explicit_salt: None,
        };
        let encrypted = encrypt(&base, &key);
        let latest = encrypt(&latest, &key);
        let tail = encrypt(&tail, &key);
        let mut header = [0; 32];
        header[..4].copy_from_slice(&0x377f0682u32.to_be_bytes());
        header[4..8].copy_from_slice(&3007000u32.to_be_bytes());
        header[8..12].copy_from_slice(&(PAGE as u32).to_be_bytes());
        header[16..24].copy_from_slice(b"12345678");
        let mut sum = checksum(&header[..24], true, (0, 0));
        header[24..28].copy_from_slice(&sum.0.to_be_bytes());
        header[28..32].copy_from_slice(&sum.1.to_be_bytes());
        let mut wal = header.to_vec();
        for (pages, committed) in [(&latest, true), (&tail, false)] {
            for (index, page) in pages.chunks_exact(PAGE).enumerate() {
                let mut frame = vec![0; 24];
                frame[..4].copy_from_slice(&(index as u32 + 1).to_be_bytes());
                frame[4..8].copy_from_slice(
                    &(if committed && (index + 1) * PAGE == pages.len() {
                        (pages.len() / PAGE) as u32
                    } else {
                        0
                    })
                    .to_be_bytes(),
                );
                frame[8..16].copy_from_slice(&header[16..24]);
                sum = checksum(page, true, checksum(&frame[..8], true, sum));
                frame[16..20].copy_from_slice(&sum.0.to_be_bytes());
                frame[20..24].copy_from_slice(&sum.1.to_be_bytes());
                wal.extend(frame);
                wal.extend(page);
            }
        }
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("encrypted.db");
        let wal_path = temp.path().join("encrypted.db-wal");
        std::fs::write(&path, &encrypted).unwrap();
        std::fs::write(&wal_path, &wal).unwrap();
        let (snapshot, snapshot_version) = snapshot(&path, &key).unwrap();
        assert_eq!(version(&path).unwrap(), Some(snapshot_version));
        assert_eq!(
            snapshot
                .query_row("SELECT value FROM fixture", [], |r| r.get::<_, String>(0))
                .unwrap(),
            "committed"
        );
        assert!(
            snapshot
                .execute("UPDATE fixture SET value='forbidden'", [])
                .is_err()
        );
        assert_eq!(std::fs::read(&path).unwrap(), encrypted);
        assert_eq!(std::fs::read(&wal_path).unwrap(), wal);
    }
}
