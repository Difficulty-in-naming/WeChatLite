#![cfg_attr(windows, windows_subsystem = "windows")]

use anyhow::{Context, Result, ensure};
use serde::Deserialize;
use std::{path::PathBuf, time::Duration};
use tokio::{
    io::{AsyncBufRead, AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader},
    sync::{mpsc, watch},
    time::timeout,
};
use wechat_p2p::{Event, Settings};

const CONFIG_LIMIT: usize = 96 * 1024;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Input {
    server_id: String,
    relay_url: String,
    relay_token: String,
    #[serde(default)]
    relay_ca: String,
    relay_quic_port: u16,
    key_file: PathBuf,
    #[serde(default)]
    relay_only: bool,
}

async fn first_line<R: AsyncBufRead + Unpin>(input: &mut R) -> Result<Vec<u8>> {
    let mut line = Vec::new();
    loop {
        let available = input.fill_buf().await?;
        ensure!(
            !available.is_empty(),
            "configuration stdin ended before its newline"
        );
        let count = available
            .iter()
            .position(|byte| *byte == b'\n')
            .map(|at| at + 1)
            .unwrap_or(available.len());
        ensure!(
            line.len() + count <= CONFIG_LIMIT,
            "configuration line exceeds limit"
        );
        line.extend_from_slice(&available[..count]);
        input.consume(count);
        if line.last() == Some(&b'\n') {
            return Ok(line);
        }
    }
}

async fn run(events: mpsc::Sender<Event>) -> Result<()> {
    ensure!(
        std::env::args().skip(1).collect::<Vec<_>>() == ["--client"],
        "expected --client"
    );
    let mut input = BufReader::new(tokio::io::stdin());
    let line = timeout(Duration::from_secs(15), first_line(&mut input))
        .await
        .context("configuration stdin timed out")??;
    let config: Input = serde_json::from_slice(&line).context("invalid helper configuration")?;
    ensure!(config.server_id.len() <= 128, "server identity is too long");
    ensure!(
        config.key_file.is_absolute() && config.key_file.as_os_str().len() <= 4096,
        "key_file must be a bounded absolute path"
    );
    let server_id = config
        .server_id
        .parse()
        .context("invalid server endpoint identity")?;
    let settings = Settings {
        enabled: true,
        relay_url: config.relay_url,
        relay_token: config.relay_token,
        relay_ca: config.relay_ca,
        relay_quic_port: config.relay_quic_port,
    };
    settings.validate()?;
    let relay = settings.relay_url.parse().context("invalid relay URL")?;
    let (exit, shutdown) = watch::channel(false);
    // EOF is an ownership boundary: closing QProcess stdin stops all tunnels and network tasks.
    let eof = tokio::spawn(async move {
        let mut discard = [0u8; 1024];
        loop {
            match input.read(&mut discard).await {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
        }
        let _ = exit.send(true);
    });
    let endpoint = wechat_p2p::bind_client(&settings, &config.key_file, config.relay_only).await?;
    let result = wechat_p2p::client(endpoint, server_id, relay, shutdown, events).await;
    eof.abort();
    result
}

#[tokio::main(worker_threads = 2)]
async fn main() {
    let (events, mut output) = mpsc::channel::<Event>(32);
    let writer = tokio::spawn(async move {
        let mut stdout = tokio::io::stdout();
        while let Some(event) = output.recv().await {
            let Ok(mut line) = serde_json::to_vec(&event) else {
                continue;
            };
            if line.len() > 1024 {
                continue;
            }
            line.push(b'\n');
            if stdout.write_all(&line).await.is_err() || stdout.flush().await.is_err() {
                break;
            }
        }
    });
    let failed = run(events.clone()).await.is_err();
    if failed {
        let _ = events.try_send(Event::Error {
            message: "P2P link failed; check endpoint identity, relay authorization and TLS certificate",
        });
    }
    drop(events);
    let _ = timeout(Duration::from_secs(2), writer).await;
    // Blocking stdio must never keep the helper alive after EOF if its owner stopped reading stdout.
    // All tunnels have already been shut down before this exit.
    std::process::exit(if failed { 1 } else { 0 });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn configuration_reader_is_bounded_and_preserves_later_bytes() {
        let mut reader = BufReader::new(&b"{}\nkept-open"[..]);
        assert_eq!(first_line(&mut reader).await.unwrap(), b"{}\n");
        let mut tail = Vec::new();
        reader.read_to_end(&mut tail).await.unwrap();
        assert_eq!(tail, b"kept-open");
        let too_long = vec![b'x'; CONFIG_LIMIT + 1];
        assert!(
            first_line(&mut BufReader::new(too_long.as_slice()))
                .await
                .is_err()
        );
        assert!(first_line(&mut BufReader::new(&b"{}"[..])).await.is_err());
    }

    #[test]
    fn no_business_key_is_accepted_by_the_helper() {
        assert!(serde_json::from_str::<Input>(r#"{"server_id":"x","relay_url":"https://127.0.0.1","relay_token":"relay-only","relay_quic_port":54443,"key_file":"/tmp/key","token":"business-secret"}"#).is_err());
    }
}
