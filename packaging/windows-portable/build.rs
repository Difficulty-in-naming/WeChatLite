use sha2::{Digest, Sha256};
use std::{env, fs::File, io::Read};

fn main() {
    println!("cargo:rerun-if-env-changed=WECHAT_PORTABLE_PAYLOAD");
    let mut hash = Sha256::new();
    if let Some(path) = env::var_os("WECHAT_PORTABLE_PAYLOAD") {
        println!(
            "cargo:rerun-if-changed={}",
            std::path::Path::new(&path).display()
        );
        let mut file = File::open(path).expect("payload file must exist");
        let mut buffer = [0u8; 65536];
        loop {
            let n = file.read(&mut buffer).expect("cannot read payload");
            if n == 0 {
                break;
            }
            hash.update(&buffer[..n]);
        }
    }
    println!("cargo:rustc-env=PORTABLE_SHA256={:x}", hash.finalize());
}
