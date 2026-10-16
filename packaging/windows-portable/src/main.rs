#![cfg_attr(windows, windows_subsystem = "windows")]

use anyhow::Result;
use std::process::Command;
use wechat_portable::{Package, install};

fn run() -> Result<()> {
    let package = Package::from_executable(&std::env::current_exe()?, env!("PORTABLE_SHA256"))?;
    let root = wechat_portable::platform::cache_root()?;
    let installed = install(&root, &package)?;
    // Keep the caller's working directory: relative --profile/--config arguments retain their meaning.
    let mut command = Command::new(installed.join(&package.manifest.entry));
    command.args(std::env::args_os().skip(1));
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(windows_sys::Win32::System::Threading::CREATE_NO_WINDOW);
    }
    command.spawn()?;
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        wechat_portable::platform::show_error(&format!(
            "微信轻聊便携包未能启动。\n\n{error:#}\n\n没有删除配置或旧版本缓存。"
        ));
        std::process::exit(1);
    }
}
