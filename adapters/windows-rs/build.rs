fn main() {
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
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        cc::Build::new()
            .cpp(true)
            .std("c++17")
            .flag_if_supported("/utf-8")
            .flag_if_supported("/EHsc")
            .flag_if_supported("/Gy")
            .file("native/chat_ui.cpp")
            .compile("wechat_chat_ui");
        for library in ["ole32", "oleaut32", "user32", "uuid"] {
            println!("cargo:rustc-link-lib={library}");
        }
    }
    println!("cargo:rerun-if-changed=native/chat_ui.cpp");
}
