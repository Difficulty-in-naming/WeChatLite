mod access;
mod actions;
mod chat_activation;
mod db;
mod engine;
mod media;
mod native;
mod quotes;
mod recall;

use anyhow::{Context, Result, ensure};
use engine::Engine;
use serde_json::{Value, json};
use std::{
    io::{self, BufRead, Read, Write},
    path::PathBuf,
    sync::mpsc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

const MAX_COMMAND: u64 = 2 * 1024 * 1024;
fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}
fn emit(value: &Value) -> Result<()> {
    let mut out = io::stdout().lock();
    serde_json::to_writer(&mut out, value)?;
    out.write_all(b"\n")?;
    out.flush()?;
    Ok(())
}

fn command<R: BufRead>(reader: &mut R) -> Result<Option<Value>> {
    let mut bytes = Vec::new();
    let length = reader.take(MAX_COMMAND + 1).read_until(b'\n', &mut bytes)?;
    if length == 0 {
        return Ok(None);
    }
    if length as u64 > MAX_COMMAND {
        while !bytes.ends_with(b"\n") {
            bytes.clear();
            let length = reader.take(8192).read_until(b'\n', &mut bytes)?;
            if length == 0 {
                break;
            }
        }
        anyhow::bail!("忽略过大的适配器命令");
    }
    let value: Value = serde_json::from_slice(&bytes).context("忽略无效的适配器命令")?;
    ensure!(value.is_object(), "适配器命令必须是对象");
    Ok(Some(value))
}

enum Input {
    Command(Value),
    Warning(String),
    End,
}
fn run() -> Result<()> {
    ensure!(cfg!(windows), "此适配器仅支持本机 Windows 微信");
    let data =
        PathBuf::from(std::env::var_os("WECHAT_LITE_DATA").unwrap_or_else(|| ".state".into()));
    std::fs::create_dir_all(&data)?;
    let data = data.canonicalize()?;
    if std::env::args().any(|arg| arg == "--probe-quote") {
        native::verify_wechat()?;
        let mut db = db::Db::connect(&data.join("adapter/wechat"))?;
        let catalog: Vec<Value> = db
            .groups()?
            .into_iter()
            .map(|mut group| {
                group["id"] = group["username"].clone();
                group
            })
            .collect();
        let policy: Value = serde_json::from_str(
            &std::env::var("WECHAT_LITE_FORWARD_GROUPS").unwrap_or_else(|_| "[]".into()),
        )?;
        let groups = engine::selection(&policy, &catalog)?;
        ensure!(!groups.is_empty(), "原生引用自检需要已选择的本机群");
        let mut quotes = quotes::Quotes::new(data.join("adapter"), &db.wxid)?;
        quotes.connect(&groups)?;
        emit(
            &json!({"ready":true,"implementation":"rust","native_quote_abi_verified":true,"sends":0}),
        )?;
        return Ok(());
    }
    if std::env::args().any(|arg| arg == "--probe") {
        native::verify_wechat()?;
        let mut db = db::Db::connect(&data.join("adapter/wechat"))?;
        let count = db.groups()?.len();
        db.release();
        emit(
            &json!({"ready":true,"groups_count":count,"platform":"windows","version":native::VERSION,"implementation":"rust","sends":0}),
        )?;
        return Ok(());
    }
    let local = access::Local::connect(data.join("adapter"))?;
    let policy = serde_json::from_str(
        &std::env::var("WECHAT_LITE_FORWARD_GROUPS").unwrap_or_else(|_| "[]".to_owned()),
    )
    .context("转发群配置JSON无效")?;
    let mut engine = Engine::new(local, data, &policy, now())?;
    emit(&json!({"type":"groups","groups":engine.catalog}))?;
    emit(&engine.status())?;
    let (sender, receiver) = mpsc::sync_channel(128);
    std::thread::spawn(move || {
        let mut reader = io::stdin().lock();
        loop {
            let input = match command(&mut reader) {
                Ok(Some(value)) => Input::Command(value),
                Ok(None) => Input::End,
                Err(error) => Input::Warning(error.to_string()),
            };
            let end = matches!(input, Input::End);
            if sender.send(input).is_err() || end {
                break;
            }
        }
    });
    let result = (|| -> Result<()> {
        loop {
            let first = match receiver.recv_timeout(Duration::from_millis(700)) {
                Ok(input) => Some(input),
                Err(mpsc::RecvTimeoutError::Timeout) => None,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            };
            let mut inputs = first.into_iter().collect::<Vec<_>>();
            inputs.extend(receiver.try_iter().take(127));
            let mut commands = vec![];
            for input in inputs {
                match input {
                    Input::End => return Ok(()),
                    Input::Warning(message) => emit(&json!({"type":"warning","text":message}))?,
                    Input::Command(value) => {
                        if value["op"] == "quit" {
                            return Ok(());
                        }
                        commands.push(value);
                    }
                }
            }
            for event in engine.cycle(&commands, now()) {
                emit(&event)?;
            }
        }
        Ok(())
    })();
    let shutdown = engine.shutdown();
    result?;
    shutdown
}

fn main() {
    if let Err(error) = run() {
        let _ = emit(&json!({"type":"status","ready":false,"text":error.to_string()}));
        std::process::exit(1);
    }
}

#[cfg(test)]
mod input_tests {
    use super::*;
    #[test]
    fn invalid_and_oversized_input_does_not_consume_the_next_command() {
        let mut input = io::Cursor::new(b"[]\n{\"op\":\"quit\"}\n");
        assert!(command(&mut input).is_err());
        assert_eq!(command(&mut input).unwrap().unwrap()["op"], "quit");
        let mut bytes = vec![b'x'; MAX_COMMAND as usize + 5];
        bytes.extend_from_slice(b"\n{\"op\":\"quit\"}\n");
        let mut input = io::Cursor::new(bytes);
        assert!(command(&mut input).is_err());
        assert_eq!(command(&mut input).unwrap().unwrap()["op"], "quit");
    }
}
