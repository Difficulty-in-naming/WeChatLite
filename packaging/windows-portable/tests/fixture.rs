use std::{
    fs::{File, OpenOptions},
    io::Write,
    path::PathBuf,
};
use wechat_portable::{Entry, MAGIC, Manifest, copy_verified, fingerprint};

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.first().is_some_and(|s| s == "--make-payload") {
        let source = std::env::current_exe().unwrap();
        let (size, sha256) = fingerprint(&mut File::open(&source).unwrap()).unwrap();
        let manifest = Manifest {
            role: "client".into(),
            entry: "wechat-lite.exe".into(),
            files: vec![Entry {
                path: "wechat-lite.exe".into(),
                size,
                sha256,
            }],
        };
        let json = serde_json::to_vec(&manifest).unwrap();
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(PathBuf::from(&args[1]))
            .unwrap();
        file.write_all(MAGIC).unwrap();
        file.write_all(&(json.len() as u64).to_le_bytes()).unwrap();
        file.write_all(&json).unwrap();
        copy_verified(&mut File::open(source).unwrap(), &mut file).unwrap();
        file.sync_all().unwrap();
        let (_, hash) = fingerprint(&mut File::open(PathBuf::from(&args[1])).unwrap()).unwrap();
        println!("{hash}");
        return;
    }
    let output = PathBuf::from(args.first().expect("fixture requires a report path"));
    let args: Vec<_> = args
        .iter()
        .map(|s| s.to_string_lossy().into_owned())
        .collect();
    let report = serde_json::json!({"args":args,"cwd":std::env::current_dir().unwrap(),"pid":std::process::id()});
    std::fs::write(output, serde_json::to_vec(&report).unwrap()).unwrap();
}
