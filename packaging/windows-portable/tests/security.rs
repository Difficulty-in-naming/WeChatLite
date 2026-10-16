use std::{
    fs::{self, File},
    io::{Seek, SeekFrom, Write},
    path::Path,
    sync::Arc,
};
use wechat_portable::{
    Entry, FOOTER, MAGIC, Manifest, Package, hash, install, open_regular, validate_path,
};

fn payload(directory: &Path, name: &str, files: &[(&str, &[u8])]) -> (std::path::PathBuf, String) {
    let manifest = Manifest {
        role: "client".into(),
        entry: "wechat-lite.exe".into(),
        files: files
            .iter()
            .map(|(path, bytes)| Entry {
                path: (*path).into(),
                size: bytes.len() as u64,
                sha256: hash(bytes),
            })
            .collect(),
    };
    let json = serde_json::to_vec(&manifest).unwrap();
    let mut bytes = MAGIC.to_vec();
    bytes.extend_from_slice(&(json.len() as u64).to_le_bytes());
    bytes.extend_from_slice(&json);
    for (_, data) in files {
        bytes.extend_from_slice(data);
    }
    let digest = hash(&bytes);
    let path = directory.join(name);
    fs::write(&path, bytes).unwrap();
    (path, digest)
}

#[test]
fn windows_traversal_devices_streams_and_ambiguous_names_are_rejected() {
    for path in [
        "",
        "../evil",
        "a/../evil",
        "a/./evil",
        "/evil",
        "C:/evil",
        "a\\evil",
        "a//evil",
        "a:stream",
        "a/evil.",
        "a/evil ",
        "NUL.txt",
        "a/COM1.dll",
        "LPT².txt",
        "a\0b",
        "a/evil?",
        "a\nb",
    ] {
        assert!(validate_path(path).is_err(), "{path:?}");
    }
    assert!(validate_path("licenses/中文说明.md").is_ok());
    assert!(validate_path("platforms/qwindows.dll").is_ok());
}

#[test]
fn full_payload_hash_truncation_duplicates_and_secret_paths_fail_closed() {
    let temp = tempfile::tempdir().unwrap();
    let (path, digest) = payload(temp.path(), "valid", &[("wechat-lite.exe", b"safe")]);
    assert!(Package::open_payload(&path, &digest).is_ok());
    assert!(Package::open_payload(&path, &"0".repeat(64)).is_err());
    for files in [
        vec![
            ("wechat-lite.exe", &b"safe"[..]),
            ("server.json", &b"secret"[..]),
        ],
        vec![
            ("wechat-lite.exe", &b"safe"[..]),
            ("platforms/q.dll", &b"a"[..]),
            ("platforms/Q.dll", &b"b"[..]),
        ],
        vec![
            ("wechat-lite.exe", &b"safe"[..]),
            ("../Qt6Core.dll", &b"a"[..]),
        ],
    ] {
        let (path, digest) = payload(temp.path(), "bad", &files);
        assert!(Package::open_payload(&path, &digest).is_err());
    }
    let mut bytes = fs::read(path).unwrap();
    bytes.pop();
    let bad = temp.path().join("truncated");
    fs::write(&bad, &bytes).unwrap();
    assert!(Package::open_payload(&bad, &hash(&bytes)).is_err());
}

#[test]
fn footer_and_package_file_hash_are_checked_before_publication() {
    let temp = tempfile::tempdir().unwrap();
    let (path, digest) = payload(temp.path(), "payload", &[("wechat-lite.exe", b"safe")]);
    let mut exe = File::create(temp.path().join("fake.exe")).unwrap();
    exe.write_all(b"fake-exe-stub").unwrap();
    let offset = exe.stream_position().unwrap();
    std::io::copy(&mut File::open(&path).unwrap(), &mut exe).unwrap();
    exe.write_all(FOOTER).unwrap();
    exe.write_all(&offset.to_le_bytes()).unwrap();
    exe.write_all(&fs::metadata(&path).unwrap().len().to_le_bytes())
        .unwrap();
    drop(exe);
    assert!(Package::from_executable(&temp.path().join("fake.exe"), &digest).is_ok());
    let mut bytes = fs::read(&path).unwrap();
    *bytes.last_mut().unwrap() ^= 1;
    fs::write(&path, &bytes).unwrap();
    let package = Package::open_payload(&path, &hash(&bytes)).unwrap();
    let root = temp.path().join("cache");
    assert!(install(&root, &package).is_err());
    assert!(!root.join("client").join(&package.hash).exists());
    let mut exe = fs::OpenOptions::new()
        .write(true)
        .open(temp.path().join("fake.exe"))
        .unwrap();
    exe.seek(SeekFrom::End(-1)).unwrap();
    exe.write_all(&[0xff]).unwrap();
    drop(exe);
    assert!(Package::from_executable(&temp.path().join("fake.exe"), &digest).is_err());
}

#[test]
fn concurrent_extraction_reuses_one_complete_cache_and_keeps_older_versions() {
    let temp = tempfile::tempdir().unwrap();
    let (path, digest) = payload(
        temp.path(),
        "one",
        &[
            ("wechat-lite.exe", b"first"),
            ("platforms/qwindows.dll", b"plugin"),
        ],
    );
    let package = Arc::new(Package::open_payload(&path, &digest).unwrap());
    let root = Arc::new(temp.path().join("cache"));
    let mut threads = Vec::new();
    for _ in 0..6 {
        let (package, root) = (package.clone(), root.clone());
        threads.push(std::thread::spawn(move || {
            install(&root, &package).unwrap()
        }));
    }
    let directories: Vec<_> = threads.into_iter().map(|t| t.join().unwrap()).collect();
    assert!(directories.iter().all(|p| p == &directories[0]));
    assert_eq!(
        fs::read(directories[0].join("platforms/qwindows.dll")).unwrap(),
        b"plugin"
    );
    let before = fs::metadata(directories[0].join("wechat-lite.exe"))
        .unwrap()
        .modified()
        .unwrap();
    assert_eq!(install(&root, &package).unwrap(), directories[0]);
    assert_eq!(
        fs::metadata(directories[0].join("wechat-lite.exe"))
            .unwrap()
            .modified()
            .unwrap(),
        before
    );
    let (next, digest) = payload(temp.path(), "two", &[("wechat-lite.exe", b"second")]);
    let next = install(&root, &Package::open_payload(&next, &digest).unwrap()).unwrap();
    assert_ne!(next, directories[0]);
    assert_eq!(
        fs::read(directories[0].join("wechat-lite.exe")).unwrap(),
        b"first"
    );
}

#[test]
fn corrupted_or_injected_cache_is_preserved_and_never_executed() {
    let temp = tempfile::tempdir().unwrap();
    let (path, digest) = payload(temp.path(), "package", &[("wechat-lite.exe", b"safe")]);
    let package = Package::open_payload(&path, &digest).unwrap();
    let root = temp.path().join("cache");
    let version = install(&root, &package).unwrap();
    fs::write(version.join("wechat-lite.exe"), b"evil").unwrap();
    fs::write(version.join("user-data.txt"), b"retain me").unwrap();
    assert_eq!(install(&root, &package).unwrap(), version);
    assert_eq!(fs::read(version.join("wechat-lite.exe")).unwrap(), b"safe");
    let preserved = fs::read_dir(root.join("client"))
        .unwrap()
        .map(|e| e.unwrap().path())
        .find(|p| {
            p.file_name()
                .unwrap()
                .to_string_lossy()
                .contains("-preserved-")
        })
        .unwrap();
    assert_eq!(
        fs::read(preserved.join("user-data.txt")).unwrap(),
        b"retain me"
    );
    fs::write(version.join("platforms-extra.dll"), b"not in manifest").unwrap();
    assert_eq!(install(&root, &package).unwrap(), version);
    assert!(!version.join("platforms-extra.dll").exists());
}

#[test]
fn hard_links_and_directory_links_are_rejected() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("file");
    fs::write(&path, b"private").unwrap();
    fs::hard_link(&path, temp.path().join("link")).unwrap();
    assert!(open_regular(&path).is_err());
    let target = temp.path().join("outside");
    fs::create_dir(&target).unwrap();
    let link = temp.path().join("cache");
    #[cfg(windows)]
    {
        let result = std::process::Command::new("powershell.exe")
            .args(["-NoProfile", "-NonInteractive", "-Command", "New-Item -ItemType Junction -Path $env:WLP_TEST_JUNCTION -Target $env:WLP_TEST_TARGET | Out-Null"])
            .env("WLP_TEST_JUNCTION", &link)
            .env("WLP_TEST_TARGET", &target)
            .output().unwrap();
        assert!(
            result.status.success(),
            "cannot create junction fixture: {}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
    #[cfg(unix)]
    std::os::unix::fs::symlink(&target, &link).unwrap();
    let (path, digest) = payload(temp.path(), "package", &[("wechat-lite.exe", b"safe")]);
    assert!(install(&link, &Package::open_payload(&path, &digest).unwrap()).is_err());
    assert_eq!(fs::read_dir(target).unwrap().count(), 0);
}

#[test]
fn native_argument_forwarding_preserves_spaces_quotes_unicode_empty_and_trailing_slashes() {
    let temp = tempfile::tempdir().unwrap();
    let output = temp.path().join("argv.json");
    let expected = [
        "",
        "带 空格",
        "a\"b",
        "tail\\",
        "two\\\"quotes",
        "https://example.invalid/a?x=1&y=2",
    ];
    let mut command = std::process::Command::new(env!("CARGO_BIN_EXE_portable-fixture"));
    command.arg(&output).args(expected).current_dir(temp.path());
    assert!(command.status().unwrap().success());
    let report: serde_json::Value = serde_json::from_slice(&fs::read(output).unwrap()).unwrap();
    assert_eq!(
        report["args"].as_array().unwrap()[1..],
        expected.map(serde_json::Value::from)
    );
    assert_eq!(Path::new(report["cwd"].as_str().unwrap()), temp.path());
}

#[test]
fn packer_rejects_configuration_and_links_before_creating_output() {
    let temp = tempfile::tempdir().unwrap();
    let source = temp.path().join("source");
    fs::create_dir(&source).unwrap();
    fs::write(source.join("server.local.json"), b"not a release file").unwrap();
    let output = temp.path().join("payload");
    let result = std::process::Command::new(env!("CARGO_BIN_EXE_portable-pack"))
        .args([
            std::ffi::OsStr::new("client"),
            source.as_os_str(),
            output.as_os_str(),
        ])
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(!output.exists());
}

#[test]
fn pe_guard_checks_gui_system_imports_and_signed_overlay_alignment() {
    let temp = tempfile::tempdir().unwrap();
    let (payload, digest) = payload(temp.path(), "payload", &[("wechat-lite.exe", b"safe")]);
    let mut bytes = vec![0u8; 1024];
    bytes[..2].copy_from_slice(b"MZ");
    bytes[60..64].copy_from_slice(&128u32.to_le_bytes());
    bytes[128..132].copy_from_slice(b"PE\0\0");
    bytes[132..134].copy_from_slice(&0x8664u16.to_le_bytes());
    bytes[134..136].copy_from_slice(&1u16.to_le_bytes());
    bytes[148..150].copy_from_slice(&240u16.to_le_bytes());
    bytes[152..154].copy_from_slice(&0x20bu16.to_le_bytes());
    bytes[220..222].copy_from_slice(&2u16.to_le_bytes());
    bytes[272..276].copy_from_slice(&0x1000u32.to_le_bytes());
    bytes[404..408].copy_from_slice(&0x1000u32.to_le_bytes());
    bytes[408..412].copy_from_slice(&512u32.to_le_bytes());
    bytes[412..416].copy_from_slice(&512u32.to_le_bytes());
    bytes[524..528].copy_from_slice(&0x1080u32.to_le_bytes());
    bytes[640..653].copy_from_slice(b"kernel32.dll\0");
    let data = fs::read(payload).unwrap();
    let offset = bytes.len() as u64;
    bytes.extend_from_slice(&data);
    bytes.extend_from_slice(FOOTER);
    bytes.extend_from_slice(&offset.to_le_bytes());
    bytes.extend_from_slice(&(data.len() as u64).to_le_bytes());
    let certificate = (bytes.len() + 7) & !7;
    bytes.resize(certificate, 0);
    bytes.extend_from_slice(&[8, 0, 0, 0, 0, 2, 2, 0]);
    bytes[296..300].copy_from_slice(&(certificate as u32).to_le_bytes());
    bytes[300..304].copy_from_slice(&8u32.to_le_bytes());
    let path = temp.path().join("synthetic-signed.exe");
    fs::write(&path, &bytes).unwrap();
    assert_eq!(
        wechat_portable::verify_launcher_pe(&path).unwrap(),
        ["kernel32.dll"]
    );
    assert!(Package::from_executable(&path, &digest).is_ok());
    bytes[220..222].copy_from_slice(&3u16.to_le_bytes());
    fs::write(&path, &bytes).unwrap();
    assert!(wechat_portable::verify_launcher_pe(&path).is_err());
    bytes[220..222].copy_from_slice(&2u16.to_le_bytes());
    bytes[640..657].copy_from_slice(b"vcruntime140.dll\0");
    fs::write(&path, &bytes).unwrap();
    assert!(wechat_portable::verify_launcher_pe(&path).is_err());
}
