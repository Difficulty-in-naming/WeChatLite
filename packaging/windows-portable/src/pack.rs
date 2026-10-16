use anyhow::{Context, Result, ensure};
use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
};
use wechat_portable::{
    Entry, MAGIC, Manifest, Package, allowed_file, check_ancestors, copy_verified, fingerprint,
    open_regular, reject_link, validate_manifest, validate_path,
};

fn collect(root: &Path, directory: &Path, paths: &mut Vec<String>, role: &str) -> Result<()> {
    for entry in fs::read_dir(directory)? {
        let path = entry?.path();
        let metadata = reject_link(&path)?;
        if metadata.is_dir() {
            collect(root, &path, paths, role)?;
        } else {
            ensure!(metadata.is_file(), "not a regular release file");
            let relative = path
                .strip_prefix(root)?
                .to_str()
                .context("release path is not Unicode")?
                .replace('\\', "/");
            validate_path(&relative)?;
            ensure!(
                allowed_file(role, &relative),
                "unexpected release file: {relative}"
            );
            paths.push(relative);
        }
    }
    Ok(())
}

fn run() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    ensure!(
        args.len() == 3,
        "usage: portable-pack <client|server> <checked-dist-directory> <new-payload-file>; or portable-pack verify <portable-exe> <payload-sha256>"
    );
    let role = args[0].to_str().context("invalid role")?;
    if role == "verify" {
        let imports = wechat_portable::verify_launcher_pe(Path::new(&args[1]))?;
        let package = Package::from_executable(
            Path::new(&args[1]),
            args[2].to_str().context("invalid hash")?,
        )?;
        println!(
            "{}",
            serde_json::json!({"verified":true,"role":package.manifest.role,"files":package.manifest.files.len(),"sha256":package.hash,"gui_subsystem":true,"system_only_imports":true,"imports":imports})
        );
        return Ok(());
    }
    ensure!(matches!(role, "client" | "server"), "invalid role");
    let input = std::path::absolute(PathBuf::from(&args[1]))?;
    check_ancestors(&input)?;
    ensure!(
        reject_link(&input)?.is_dir(),
        "input must be a checked runtime directory"
    );
    let root = fs::canonicalize(input)?;
    let output = std::path::absolute(PathBuf::from(&args[2]))?;
    check_ancestors(output.parent().context("output needs a parent")?)?;
    let mut paths = Vec::new();
    collect(&root, &root, &mut paths, role)?;
    paths.sort();
    for required in [
        "qt.conf",
        "THIRD_PARTY_NOTICES.md",
        "使用说明.md",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Network.dll",
        "Qt6WebSockets.dll",
        "Qt6Widgets.dll",
        "vcruntime140.dll",
        "platforms/qwindows.dll",
    ] {
        ensure!(
            paths.iter().any(|p| p == required),
            "required runtime file missing: {required}"
        );
    }
    ensure!(
        paths.iter().any(|p| p.starts_with("licenses/")),
        "license texts are missing"
    );
    let executables = if role == "client" {
        vec!["wechat-lite.exe", "wechat-link.exe"]
    } else {
        vec![
            "wechat-server.exe",
            "wechat-relay.exe",
            "adapter/wechat-adapter.exe",
            "adapter/WeChatActions.dll",
            "adapter/WeChatQuote.dll",
            "adapter/ffmpeg.exe",
            "thirdparty-sources/ffmpeg-7.1.tar.xz",
            "thirdparty-sources/pilk-0.2.4.tar.gz",
        ]
    };
    for required in executables {
        ensure!(
            paths.iter().any(|p| p == required),
            "required release file missing: {required}"
        );
    }
    let mut entries = Vec::new();
    for path in &paths {
        let (size, sha256) = fingerprint(&mut open_regular(&root.join(path))?)?;
        entries.push(Entry {
            path: path.clone(),
            size,
            sha256,
        });
    }
    let manifest = Manifest {
        role: role.to_owned(),
        entry: if role == "client" {
            "wechat-lite.exe"
        } else {
            "wechat-server.exe"
        }
        .into(),
        files: entries,
    };
    validate_manifest(&manifest)?;
    let json = serde_json::to_vec(&manifest)?;
    let mut destination = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&output)?;
    destination.write_all(MAGIC)?;
    destination.write_all(&(json.len() as u64).to_le_bytes())?;
    destination.write_all(&json)?;
    for entry in &manifest.files {
        let source = root.join(&entry.path);
        check_ancestors(&source)?;
        let (size, sha256) = copy_verified(&mut open_regular(&source)?, &mut destination)?;
        ensure!(
            size == entry.size && sha256 == entry.sha256,
            "source changed while packaging"
        );
    }
    destination.sync_all()?;
    drop(destination);
    let (size, sha256) = fingerprint(&mut File::open(&output)?)?;
    Package::open_payload(&output, &sha256)?;
    println!(
        "{}",
        serde_json::json!({"role":role,"files":manifest.files.len(),"size":size,"sha256":sha256})
    );
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error:#}");
        std::process::exit(1);
    }
}
