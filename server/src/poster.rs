//! Small, on-demand video covers. Never delay message delivery for a preview.
use crate::{App, model::Media};
use anyhow::{Context, Result, ensure};
use sha2::{Digest, Sha256};
use std::{
    path::{Path, PathBuf},
    process::Stdio,
    time::Duration,
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

pub const MAX_BYTES: usize = 48 * 1024;

fn jpeg(data: &[u8]) -> bool {
    if data.len() > MAX_BYTES || !data.starts_with(&[0xff, 0xd8]) || !data.ends_with(&[0xff, 0xd9])
    {
        return false;
    }
    let mut offset = 2;
    while offset + 4 <= data.len() {
        if data[offset] != 0xff {
            return false;
        }
        let marker = data[offset + 1];
        offset += 2;
        if marker == 0xff {
            offset -= 1;
            continue;
        }
        let length = u16::from_be_bytes([data[offset], data[offset + 1]]) as usize;
        if length < 2 || offset + length > data.len() {
            return false;
        }
        if matches!(marker, 0xc0 | 0xc1 | 0xc2) {
            if length < 8 {
                return false;
            }
            let height = u16::from_be_bytes([data[offset + 3], data[offset + 4]]);
            let width = u16::from_be_bytes([data[offset + 5], data[offset + 6]]);
            return (1..=480).contains(&width) && (1..=288).contains(&height);
        }
        offset += length;
    }
    false
}

fn decoder(app: &App) -> Result<PathBuf> {
    let parent = std::env::current_exe()?
        .parent()
        .context("程序目录不可用")?
        .to_owned();
    let bridge = Path::new(&app.config.bridge_program)
        .parent()
        .unwrap_or(&parent);
    for path in [
        bridge.join("ffmpeg.exe"),
        bridge.join("ffmpeg"),
        parent.join("adapter/ffmpeg.exe"),
        parent.join("ffmpeg"),
    ] {
        if path.is_file() {
            return Ok(path);
        }
    }
    anyhow::bail!("视频封面解码器暂不可用")
}

async fn read_cached(path: &Path) -> Option<Vec<u8>> {
    let info = tokio::fs::symlink_metadata(path).await.ok()?;
    if !info.is_file() || info.len() > MAX_BYTES as u64 {
        return None;
    }
    let file = tokio::fs::File::open(path).await.ok()?;
    let mut bytes = Vec::new();
    file.take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .await
        .ok()?;
    jpeg(&bytes).then_some(bytes)
}

async fn extract(exe: &Path, path: &Path) -> Result<Vec<u8>> {
    let mut command = tokio::process::Command::new(exe);
    command
        .args([
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-max_alloc",
            "67108864",
            "-threads",
            "1",
            "-max_pixels",
            "16777216",
            "-protocol_whitelist",
            "file,pipe",
            "-format_whitelist",
            "mov,matroska,webm,avi,mpegts,mpeg,m4v",
            "-probesize",
            "1048576",
            "-analyzeduration",
            "1000000",
            "-i",
        ])
        .arg(path)
        .args([
            "-map",
            "0:v:0",
            "-an",
            "-sn",
            "-dn",
            "-frames:v",
            "1",
            "-vf",
            "scale=480:288:force_original_aspect_ratio=decrease",
            "-filter_threads",
            "1",
            "-threads",
            "1",
            "-q:v",
            "6",
            "-c:v",
            "mjpeg",
            "-f",
            "image2pipe",
            "pipe:1",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true);
    #[cfg(windows)]
    command.creation_flags(0x00000008); // Keep one short-lived decoder free of a console host.
    let mut child = command.spawn().context("无法生成视频封面")?;
    let output = child.stdout.take().context("视频封面输出不可用")?;
    let result = tokio::time::timeout(Duration::from_secs(3), async {
        let mut bytes = Vec::new();
        output
            .take(MAX_BYTES as u64 + 1)
            .read_to_end(&mut bytes)
            .await?;
        ensure!(bytes.len() <= MAX_BYTES, "视频封面超过容量");
        ensure!(
            child.wait().await?.success() && jpeg(&bytes),
            "此视频暂无法生成封面"
        );
        Ok(bytes)
    })
    .await
    .context("生成视频封面超时")
    .and_then(|value| value);
    if result.is_err() {
        let _ = child.kill().await;
        let _ = child.wait().await;
    }
    result
}

pub async fn load(app: &App, media: &Media) -> Result<Vec<u8>> {
    ensure!(media.kind == "video", "此附件不是视频");
    let source = tokio::fs::canonicalize(&media.path).await?;
    let media_root = tokio::fs::canonicalize(app.config.data.join("media")).await?;
    ensure!(source.starts_with(media_root), "视频路径无效");
    let directory = app.config.data.join("video-posters");
    tokio::fs::create_dir_all(&directory).await?;
    let directory = tokio::fs::canonicalize(directory).await?;
    ensure!(
        directory.starts_with(tokio::fs::canonicalize(&app.config.data).await?),
        "封面缓存路径无效"
    );
    let key = format!(
        "{:x}",
        Sha256::digest(format!("{}:{}", media.id, media.sha256).as_bytes())
    );
    let target = directory.join(format!("{key}.jpg"));
    if let Some(bytes) = read_cached(&target).await {
        return Ok(bytes);
    }
    let _permit = tokio::time::timeout(Duration::from_secs(6), app.posters.acquire()).await??;
    if let Some(bytes) = read_cached(&target).await {
        return Ok(bytes);
    }
    let exe = decoder(app)?;
    let bytes = tokio::select! {
        _ = crate::wait_for_shutdown(app.shutdown.subscribe()) => anyhow::bail!("服务正在退出"),
        result = extract(&exe, &source) => result?,
    };
    let partial = crate::web::PartialFile(directory.join(format!("{}.part", uuid::Uuid::new_v4())));
    let mut file = tokio::fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&partial.0)
        .await?;
    file.write_all(&bytes).await?;
    file.flush().await?;
    drop(file);
    tokio::fs::rename(&partial.0, &target).await?;
    partial.keep();
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    pub fn small_jpeg() -> Vec<u8> {
        vec![
            0xff, 0xd8, 0xff, 0xc0, 0, 8, 8, 0, 90, 0, 160, 1, 0xff, 0xd9,
        ]
    }

    #[test]
    fn jpeg_headers_and_capacity_are_bounded() {
        assert!(jpeg(&small_jpeg()));
        for bytes in [vec![], vec![0xff, 0xd8, 0xff], vec![0; MAX_BYTES + 1]] {
            assert!(!jpeg(&bytes));
        }
        let mut huge = small_jpeg();
        huge[9] = 10;
        assert!(!jpeg(&huge));
        let mut truncated = small_jpeg();
        truncated[5] = 100;
        assert!(!jpeg(&truncated));
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn real_decoder_extracts_one_small_frame_and_rejects_bad_input() {
        let exe = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../.tools/Lib/site-packages/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe");
        assert!(
            exe.is_file(),
            "Windows validation requires the packaged FFmpeg"
        );
        let directory = tempfile::tempdir().unwrap();
        let input = directory.path().join("offline.mp4");
        assert!(
            tokio::process::Command::new(&exe)
                .args([
                    "-nostdin",
                    "-loglevel",
                    "error",
                    "-f",
                    "lavfi",
                    "-i",
                    "color=c=green:s=320x180:r=10",
                    "-t",
                    "1",
                    "-c:v",
                    "libx264",
                    "-threads",
                    "1"
                ])
                .arg(&input)
                .status()
                .await
                .unwrap()
                .success()
        );
        let cover = extract(&exe, &input).await.unwrap();
        assert!(jpeg(&cover));
        assert!(cover.len() < 16 * 1024);
        tokio::fs::write(&input, b"invalid video").await.unwrap();
        assert!(extract(&exe, &input).await.is_err());
    }
}
