fn main() {
    use sha2::{Digest, Sha256};
    use std::{env, fs, path::PathBuf};

    println!("cargo:rerun-if-env-changed=WECHAT_MEDIA_DLL");
    println!("cargo:rerun-if-env-changed=WECHAT_MEDIA_DLL_SHA256");
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let (media, expected) = match (
        env::var_os("WECHAT_MEDIA_DLL"),
        env::var("WECHAT_MEDIA_DLL_SHA256").ok(),
    ) {
        (Some(path), Some(hash)) => (PathBuf::from(path), hash),
        (None, None) => {
            let checksum = manifest.join("vendor/WeChatMedia.sha256");
            println!("cargo:rerun-if-changed={}", checksum.display());
            (
                manifest.join("vendor/WeChatMedia.dll"),
                fs::read_to_string(checksum).unwrap(),
            )
        }
        _ => panic!("WECHAT_MEDIA_DLL and WECHAT_MEDIA_DLL_SHA256 must be set together"),
    };
    let media = media.canonicalize().expect("native media DLL path");
    let bytes = fs::read(&media).expect("native media DLL bytes");
    let actual = format!("{:x}", Sha256::digest(&bytes));
    assert!(bytes.starts_with(b"MZ"), "native media DLL must be a PE file");
    assert_eq!(
        actual,
        expected.trim(),
        "native media DLL fingerprint mismatch"
    );
    println!("cargo:rerun-if-changed={}", media.display());
    println!("cargo:rustc-env=WECHAT_MEDIA_BINARY_PATH={}", media.display());
    println!("cargo:rustc-env=WECHAT_MEDIA_BINARY_SHA256={actual}");

    let root = std::path::Path::new("vendor/pilk-0.2.4/src");
    let mut build = cc::Build::new();
    build
        .include(root.join("SKP_SILK_SRC"))
        .include(root.join("interface"))
        .warnings(false)
        .opt_level(2)
        .define("_CRT_SECURE_NO_WARNINGS", None);
    let mut sources: Vec<_> = std::fs::read_dir(root.join("SKP_SILK_SRC"))
        .expect("bundled SILK SDK")
        .map(|entry| entry.expect("SILK source").path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "c"))
        .collect();
    sources.sort();
    for source in sources {
        println!("cargo:rerun-if-changed={}", source.display());
        build.file(source);
    }
    build.compile("wechat_silk");
    println!("cargo:rerun-if-changed=vendor/pilk-0.2.4/src/interface");
    println!("cargo:rerun-if-changed=vendor/pilk-0.2.4/src/SKP_SILK_SRC");
}
