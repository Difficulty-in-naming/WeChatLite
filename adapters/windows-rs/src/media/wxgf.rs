//! Static WXGF consists of independent HEVC color and optional grayscale alpha.
//! Never treat its alpha stream as a second animation frame.
use anyhow::{Context, Result, ensure};
use image::{DynamicImage, ImageFormat};
use std::{
    fs,
    io::Cursor,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

fn nals(data: &[u8]) -> Vec<(u8, &[u8])> {
    let mut starts = Vec::new();
    let mut i = 0;
    while i + 3 <= data.len() {
        let size = if data.get(i..i + 4) == Some(&[0, 0, 0, 1]) {
            4
        } else if data.get(i..i + 3) == Some(&[0, 0, 1]) {
            3
        } else {
            0
        };
        if size > 0 {
            starts.push((i, i + size));
            i += size;
        } else {
            i += 1;
        }
    }
    starts
        .iter()
        .enumerate()
        .filter_map(|(i, (_, begin))| {
            let end = starts
                .get(i + 1)
                .map(|(offset, _)| *offset)
                .unwrap_or(data.len());
            let unit = &data[*begin..end];
            (unit.len() >= 2).then_some(((unit[0] >> 1) & 63, unit))
        })
        .collect()
}
struct Bits {
    data: Vec<u8>,
    position: usize,
}
impl Bits {
    fn take(&mut self, count: usize) -> Result<u32> {
        ensure!(
            count <= 32 && self.position + count <= self.data.len() * 8,
            "HEVC 参数截断"
        );
        let mut value = 0;
        for _ in 0..count {
            value = (value << 1)
                | u32::from((self.data[self.position / 8] >> (7 - self.position % 8)) & 1);
            self.position += 1;
        }
        Ok(value)
    }
    fn skip(&mut self, count: usize) -> Result<()> {
        ensure!(
            self.position + count <= self.data.len() * 8,
            "HEVC 参数截断"
        );
        self.position += count;
        Ok(())
    }
    fn ue(&mut self) -> Result<u32> {
        let mut zeros = 0;
        while self.take(1)? == 0 {
            zeros += 1;
            ensure!(zeros <= 31, "HEVC 参数无效");
        }
        Ok((1u32 << zeros) - 1 + self.take(zeros)?)
    }
}
fn chroma(stream: &[u8]) -> Result<u32> {
    let units = nals(stream);
    let sps = units
        .iter()
        .find(|(kind, _)| *kind == 33)
        .context("缺少 HEVC 参数")?
        .1;
    ensure!(sps.len() <= 1024 * 1024, "HEVC 参数过大");
    let mut rbsp = Vec::new();
    let mut i = 2;
    while i < sps.len() {
        if sps.get(i..i + 3) == Some(&[0, 0, 3]) {
            rbsp.extend_from_slice(&[0, 0]);
            i += 3;
        } else {
            rbsp.push(sps[i]);
            i += 1;
        }
    }
    let mut bits = Bits {
        data: rbsp,
        position: 0,
    };
    bits.skip(4)?;
    let layers = bits.take(3)? as usize;
    bits.skip(1 + 96)?;
    let mut flags = Vec::new();
    for _ in 0..layers {
        flags.push((bits.take(1)? != 0, bits.take(1)? != 0));
    }
    if layers > 0 {
        bits.skip(2 * (8 - layers))?;
    }
    for (profile, level) in flags {
        if profile {
            bits.skip(88)?;
        }
        if level {
            bits.skip(8)?;
        }
    }
    bits.ue()?;
    let chroma = bits.ue()?;
    ensure!(chroma <= 3, "HEVC 颜色格式无效");
    if chroma == 3 {
        bits.skip(1)?;
    }
    let width = bits.ue()?;
    let height = bits.ue()?;
    ensure!(
        width > 0 && height > 0 && width <= 12000 && height <= 12000,
        "WXGF 像素范围无效"
    );
    Ok(chroma)
}
fn partitions(data: &[u8]) -> Result<Vec<(u32, &[u8])>> {
    let start = (0..data.len().saturating_sub(5))
        .find(|i| {
            data.get(*i..*i + 6) == Some(&[0, 0, 0, 1, 0x40, 1])
                || data.get(*i..*i + 5) == Some(&[0, 0, 1, 0x40, 1])
        })
        .context("缺少 WXGF 图像码流")?;
    ensure!(start >= 4, "WXGF 分片头无效");
    let mut pos = start - 4;
    let mut out = Vec::new();
    while pos < data.len() {
        let size = u32::from_be_bytes(
            data.get(pos..pos + 4)
                .context("WXGF 分片尚未完整写入")?
                .try_into()
                .unwrap(),
        ) as usize;
        pos += 4;
        ensure!(
            size > 0 && size <= data.len() - pos && out.len() < 2,
            "WXGF 分片不完整或暂不支持"
        );
        let stream = &data[pos..pos + size];
        let count = nals(stream)
            .iter()
            .filter(|(kind, unit)| *kind < 32 && unit.len() > 2 && unit[2] & 128 != 0)
            .count();
        ensure!(count == 1, "WXGF 多帧图像暂不支持，原消息已保留");
        out.push((chroma(stream)?, stream));
        pos += size;
    }
    ensure!(!out.is_empty(), "WXGF 图像码流缺失");
    Ok(out)
}
fn ffmpeg() -> Result<PathBuf> {
    let exe = std::env::current_exe()?;
    let parent = exe.parent().context("程序目录不可用")?;
    for candidate in [
        parent.join("ffmpeg.exe"),
        parent.join("ffmpeg"),
        parent.join("tools/ffmpeg.exe"),
    ] {
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    #[cfg(test)]
    {
        let candidate = Path::new(env!("CARGO_MANIFEST_DIR")).join(
            "../../.tools/Lib/site-packages/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe",
        );
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    anyhow::bail!("缺少独立 FFmpeg 解码器，WXGF 原消息已保留")
}
fn run_frame(exe: &Path, stream: &[u8], directory: &Path, name: &str) -> Result<DynamicImage> {
    let input = directory.join(format!("{name}.hevc"));
    let output = directory.join(format!("{name}.png"));
    fs::write(&input, stream)?;
    let mut command = Command::new(exe);
    command
        .args([
            "-nostdin",
            "-v",
            "error",
            "-xerror",
            "-threads",
            "1",
            "-max_alloc",
            "268435456",
            "-f",
            "hevc",
            "-i",
        ])
        .arg(&input)
        .args([
            "-frames:v",
            "1",
            "-threads",
            "1",
            "-f",
            "image2",
            "-vcodec",
            "png",
            "-y",
        ])
        .arg(&output)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command.spawn().context("无法启动 WXGF 解码器")?;
    let start = Instant::now();
    loop {
        if let Some(status) = child.try_wait()? {
            ensure!(status.success(), "WXGF 图像未完整解码");
            break;
        }
        if start.elapsed() > Duration::from_secs(30)
            || fs::metadata(&output).is_ok_and(|m| m.len() > super::MAX_IMAGE)
        {
            let _ = child.kill();
            let _ = child.wait();
            anyhow::bail!("WXGF 图像解码超时或超过范围");
        }
        thread::sleep(Duration::from_millis(20));
    }
    let data = super::read_limited(&output, super::MAX_IMAGE)?;
    super::picture::validate(&data, 12000)?;
    Ok(image::load_from_memory(&data)?)
}
pub(super) fn decode(data: &[u8], directory: &Path) -> Result<Vec<u8>> {
    let result = (|| {
        ensure!(
            data.starts_with(b"wxgf") && data.len() as u64 <= super::MAX_IMAGE,
            "WXGF 图片无效或过大"
        );
        let parts = partitions(data)?;
        let (color, mask) = if parts.len() == 2 {
            let colors: Vec<_> = parts.iter().filter(|(chroma, _)| *chroma != 0).collect();
            let masks: Vec<_> = parts.iter().filter(|(chroma, _)| *chroma == 0).collect();
            ensure!(
                colors.len() == 1 && masks.len() == 1,
                "无法区分 WXGF 颜色和遮罩"
            );
            (colors[0].1, Some(masks[0].1))
        } else {
            (parts[0].1, None)
        };
        let exe = ffmpeg()?;
        let temporary = directory.join(format!("wxgf-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&temporary)?;
        let decoded = (|| {
            let mut picture = run_frame(&exe, color, &temporary, "color")?.into_rgba8();
            if let Some(mask) = mask {
                let mask = run_frame(&exe, mask, &temporary, "alpha")?.into_luma8();
                ensure!(
                    mask.dimensions() == picture.dimensions(),
                    "WXGF 颜色和遮罩尺寸不一致"
                );
                for (x, y, pixel) in picture.enumerate_pixels_mut() {
                    pixel[3] = mask.get_pixel(x, y)[0];
                }
            }
            let mut out = Cursor::new(Vec::new());
            DynamicImage::ImageRgba8(picture).write_to(&mut out, ImageFormat::Png)?;
            ensure!(
                out.get_ref().len() as u64 <= super::MAX_IMAGE,
                "WXGF 解码图像超过 10 MiB"
            );
            Ok(out.into_inner())
        })();
        for name in ["color.hevc", "color.png", "alpha.hevc", "alpha.png"] {
            let _ = fs::remove_file(temporary.join(name));
        }
        let _ = fs::remove_dir(&temporary);
        decoded
    })();
    result.map_err(|_| super::pending("微信 WXGF 图片尚未完整解码，原消息已保留，会自动重试"))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn synthetic_hevc(exe: &Path, folder: &Path, name: &str, color: &str, gray: bool) -> Vec<u8> {
        let output = folder.join(format!("{name}.hevc"));
        let mut command = Command::new(exe);
        command
            .args([
                "-nostdin",
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                color,
                "-frames:v",
                "1",
                "-c:v",
                "libx265",
                "-preset",
                "ultrafast",
                "-x265-params",
                "pools=none:frame-threads=1:log-level=error",
                "-pix_fmt",
                if gray { "gray" } else { "yuv420p" },
                "-f",
                "hevc",
                "-y",
            ])
            .arg(&output)
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        assert!(command.status().unwrap().success());
        fs::read(output).unwrap()
    }
    #[test]
    fn real_static_hevc_color_and_alpha_are_combined_instead_of_first_frame() {
        let root = tempfile::tempdir().unwrap();
        let exe = ffmpeg().unwrap();
        let color = synthetic_hevc(
            &exe,
            root.path(),
            "red",
            "color=c=red:size=32x24:rate=1",
            false,
        );
        let mask = synthetic_hevc(
            &exe,
            root.path(),
            "mask",
            "color=c=gray:size=32x24:rate=1",
            true,
        );
        let mut wxgf = b"wxgf\0\0\0\0".to_vec();
        for stream in [&mask, &color] {
            wxgf.extend_from_slice(&(stream.len() as u32).to_be_bytes());
            wxgf.extend_from_slice(stream);
        }
        let output = decode(&wxgf, root.path()).unwrap();
        let picture = image::load_from_memory(&output).unwrap().into_rgba8();
        assert_eq!(picture.dimensions(), (32, 24));
        let pixel = picture.get_pixel(10, 10);
        assert!(pixel[0] > 200 && pixel[1] < 40 && pixel[2] < 40);
        assert!((100..=160).contains(&pixel[3]));
        assert!(decode(&wxgf[..wxgf.len() - 5], root.path()).is_err());
    }
    #[test]
    fn unknown_wxgf_stays_pending() {
        let root = tempfile::tempdir().unwrap();
        let error = decode(b"wxgfnot-a-supported-container", root.path()).unwrap_err();
        assert!(error.downcast_ref::<super::super::MediaPending>().is_some());
    }
}
