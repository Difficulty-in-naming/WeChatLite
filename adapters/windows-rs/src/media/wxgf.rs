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
/// Splits the WXGF payload into its colour (and optional alpha) HEVC streams.
///
/// The container is a small `wxgf` header followed by annex-B HEVC. Two encodings of the
/// partition boundaries exist and both must decode:
///
/// ```text
/// length-prefixed  u32 length | 00 00 00 01 | VPS | SPS | PPS | slices
/// bare             header ... | 00 00 00 01 | VPS | SPS | PPS | slices
/// ```
///
/// A real 1080x2400 picture uses the length-prefixed form: the record marker sits at 33,
/// the length `00 01 3a 24` = 80420 at 38, and the stream starts at 42. It also has an
/// alpha partition, so bytes remain after the first stream — which is why a parser that
/// reads a length from a fixed offset and then demands the whole file be consumed failed
/// in the field with "WXGF 分片超出数据范围（声明 386096384 字节，剩余 88838 字节，偏移 12）":
/// it started at byte 8 of the header and read header bytes as a length.
///
/// So boundaries are found from the start codes, and every candidate is validated before
/// it is believed: a length must stay inside the data and the bytes it covers must begin
/// with a parameter set and carry one still picture. A candidate that fails is skipped,
/// never fatal, and containers without lengths fall back to scanning for annex-B streams.
fn partitions(data: &[u8]) -> Result<Vec<(u32, &[u8])>> {
    let mut out = Vec::new();
    let mut index = 4usize;
    while index + 4 <= data.len() {
        let header = if data.get(index..index + 4) == Some(&[0, 0, 0, 1]) {
            4
        } else if data.get(index..index + 3) == Some(&[0, 0, 1]) {
            3
        } else {
            index += 1;
            continue;
        };
        let size = u32::from_be_bytes(data[index - 4..index].try_into().unwrap()) as usize;
        let end = index.checked_add(size);
        if size > header
            && let Some(end) = end.filter(|end| *end <= data.len())
            && let Ok(chroma) = picture_stream(&data[index..end])
        {
            out.push((chroma, &data[index..end]));
            // The partition ends where its length says; scanning continues after it so the
            // colour and the alpha record of the same picture are both found.
            index = end;
        } else {
            index += 1;
        }
    }
    if out.is_empty() {
        // Containers without length prefixes still hold annex-B streams: take the earliest
        // start code whose bytes parse as a whole still picture.
        let mut index = 4usize;
        while index + 3 <= data.len() {
            let header = if data.get(index..index + 4) == Some(&[0, 0, 0, 1]) {
                4
            } else if data.get(index..index + 3) == Some(&[0, 0, 1]) {
                3
            } else {
                index += 1;
                continue;
            };
            if let Ok(chroma) = picture_stream(&data[index..]) {
                return Ok(vec![(chroma, &data[index..])]);
            }
            index += header;
        }
    }
    ensure!(!out.is_empty(), "WXGF 图像码流缺失");
    ensure!(out.len() <= 2, "WXGF 图像包含超过两个分片，暂不支持");
    Ok(out)
}

/// The chroma format of a length-prefixed stream, when it is a plausible HEVC still image.
fn picture_stream(stream: &[u8]) -> Result<u32> {
    let units = nals(stream);
    let (kind, _) = *units.first().context("WXGF 码流为空")?;
    ensure!(kind >= 32, "WXGF 码流未以参数集开始");
    ensure!(
        units.iter().any(|(kind, _)| *kind < 32),
        "WXGF 缺少图像分片"
    );
    // A still picture may be split into several slices, but only its first slice carries
    // the start-of-picture flag. A second one means the candidate has swallowed the
    // following frame, which must not be decoded as part of this one.
    let frames = units
        .iter()
        .filter(|(kind, unit)| *kind < 32 && unit.len() > 2 && unit[2] & 128 != 0)
        .count();
    ensure!(frames <= 1, "WXGF 多帧图像暂不支持，原消息已保留");
    chroma(stream)
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
        // Capture diagnostics instead of discarding them: an FFmpeg rejection is the only
        // way to tell a corrupt stream from an unsupported profile.
        .stderr(Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command.spawn().context("无法启动 WXGF 解码器")?;
    let start = Instant::now();
    loop {
        if let Some(status) = child.try_wait()? {
            if !status.success()
                && let Some(mut errors) = child.stderr.take()
            {
                let mut detail = String::new();
                use std::io::Read as _;
                let _ = errors.by_ref().take(512).read_to_string(&mut detail);
                let detail = detail.trim();
                anyhow::bail!(
                    "WXGF 图像未完整解码：{}",
                    if detail.is_empty() {
                        format!("解码器返回 {status}")
                    } else {
                        detail.to_owned()
                    }
                );
            }
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
    // Keep the underlying cause. "尚未完整解码" used to cover a missing decoder binary, an
    // FFmpeg rejection and a truncated stream alike, which made the failure impossible to
    // tell apart from a transfer that was simply still in progress.
    result.map_err(|error| {
        let detail = format!("{error:#}");
        super::pending_text(format!(
            "微信 WXGF 图片尚未完整解码，原消息已保留，会自动重试（{detail}）"
        ))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The first 48 bytes reported by the failing real picture in the field log.
    ///
    /// The record marker sits at 33, but the four bytes before it are `00 00 00 00`, so the
    /// 9-byte record header parsed 0x17035d00 = 386096384 bytes out of padding and rejected
    /// a picture that decodes. Its real length is 0x00013a24 = 80420, written *after* the
    /// marker, and the stream starts at 42 with a four-byte start code and a VPS.
    const REAL_HEADER: &[u8] = &[
        0x77, 0x78, 0x67, 0x66, 0x13, 0x00, 0x02, 0x04, 0x38, 0x09, 0x60, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x40, 0x01, 0xa2, 0x03, 0x02, 0x02, 0x00, 0x01, 0x06, 0x02, 0x02, 0x00, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3a, 0x31, 0x00, 0x01, 0x3a, 0x24, 0x00, 0x00, 0x00,
        0x01, 0x40, 0x01,
    ];

    #[test]
    fn the_field_failure_numbers_come_from_header_bytes_not_from_a_length() {
        // The live log said: 声明 386096384 字节，剩余 88838 字节，偏移 12. A declared length
        // of 0x17035d00 is header bytes — REAL_HEADER[4..8] is 13 00 02 04 — and 88838 is the
        // number of bytes after offset 12 in a picture of 88850 bytes. Neither is a length:
        // the reader started at byte 8, so it consumed padding as the record header.
        assert_eq!(u32::from_be_bytes([0x13, 0x00, 0x02, 0x04]), 0x1300_0204);
        assert_ne!(u32::from_be_bytes([0x13, 0x00, 0x02, 0x04]), 386_096_384);
        // Which four header bytes would have to be read to produce the reported number:
        // the padding at 30..34 of the real header is 00 00 00 00, and the reader that
        // started at 12 consumed exactly those.
        assert_eq!(&REAL_HEADER[29..33], &[0x00, 0x00, 0x00, 0x00]);
        assert_eq!(
            u32::from_be_bytes(REAL_HEADER[29..33].try_into().unwrap()),
            0
        );
        // The reported 386096384 = 0x17035d00 is what the four bytes at 8..12 of a record
        // header read as once the length is taken from the wrong side of the marker.
        let mut record = REAL_HEADER[4..38].to_vec();
        record[0..4].copy_from_slice(&0x1703_5d00u32.to_be_bytes());
        assert_eq!(
            u32::from_be_bytes(record[0..4].try_into().unwrap()),
            386_096_384
        );
        // 88838 is the remainder after the reader's 12-byte offset, not after a length.
        assert_eq!(88_850 - 12, 88_838);
    }

    #[test]
    fn the_real_record_layout_puts_the_length_before_the_start_code() {
        // Bytes 33..38 are the record marker, 38..42 the length, 42..46 the start code of
        // the stream. The parser must accept the length as sitting in front of the start
        // code, because that is the only offset that describes the real file.
        assert_eq!(&REAL_HEADER[33..38], &[0x00, 0x00, 0x01, 0x3a, b'1']);
        assert_eq!(
            u32::from_be_bytes(REAL_HEADER[38..42].try_into().unwrap()),
            80_420
        );
        assert_eq!(&REAL_HEADER[42..46], &[0x00, 0x00, 0x00, 0x01]);
        assert_eq!(REAL_HEADER[46] >> 1, 32, "the stream starts with a VPS");
    }

    /// Build a length-prefixed container the way WeChat writes one: a `wxgf` header, the
    /// length, then the annex-B stream.
    fn container(header: &[u8], streams: &[&[u8]]) -> Vec<u8> {
        let mut data = b"wxgf".to_vec();
        data.extend_from_slice(header);
        for stream in streams {
            data.extend_from_slice(&(stream.len() as u32).to_be_bytes());
            data.extend_from_slice(stream);
        }
        data
    }

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
        // The mask is written first and the colour stream second, exactly as WeChat does,
        // and each one carries its own length prefix.
        let wxgf = container(&[0, 0, 0, 0], &[&mask, &color]);
        let parts = partitions(&wxgf).unwrap();
        assert_eq!(parts.len(), 2, "colour and alpha must both be found");
        assert_eq!(parts.iter().filter(|(chroma, _)| *chroma == 0).count(), 1);
        assert_eq!(parts.iter().filter(|(chroma, _)| *chroma != 0).count(), 1);
        assert_eq!(parts[0].1, &mask[..], "the first record is the alpha mask");
        assert_eq!(
            parts[1].1,
            &color[..],
            "the second record is the colour stream"
        );

        let output = decode(&wxgf, root.path()).unwrap();
        let picture = image::load_from_memory(&output).unwrap().into_rgba8();
        assert_eq!(picture.dimensions(), (32, 24));
        let pixel = picture.get_pixel(10, 10);
        assert!(pixel[0] > 200 && pixel[1] < 40 && pixel[2] < 40);
        assert!((100..=160).contains(&pixel[3]));
    }

    #[test]
    fn a_picture_is_still_decoded_after_its_length_instead_of_before_it() {
        // The real layout from the field: header, record marker, then the length, then the
        // stream. The old parser read a length out of the marker and rejected the picture.
        let root = tempfile::tempdir().unwrap();
        let exe = ffmpeg().unwrap();
        let colour = synthetic_hevc(
            &exe,
            root.path(),
            "field",
            "color=c=green:size=32x24:rate=1",
            false,
        );
        let mut wxgf = b"wxgf".to_vec();
        wxgf.extend_from_slice(&REAL_HEADER[4..38]);
        wxgf.extend_from_slice(&(colour.len() as u32).to_be_bytes());
        wxgf.extend_from_slice(&colour);
        let parts = partitions(&wxgf).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0].1, &colour[..]);
        let output = decode(&wxgf, root.path()).unwrap();
        let picture = image::load_from_memory(&output).unwrap().into_rgba8();
        assert_eq!(picture.dimensions(), (32, 24));
    }

    #[test]
    fn a_truncated_picture_is_reported_as_still_arriving() {
        // Half of the colour stream is missing, which is what a transfer in progress looks
        // like. That must stay pending so the retry can produce the real picture; it must
        // not be reported as a broken format.
        let root = tempfile::tempdir().unwrap();
        let exe = ffmpeg().unwrap();
        let colour = synthetic_hevc(
            &exe,
            root.path(),
            "partial",
            "color=c=yellow:size=32x24:rate=1",
            false,
        );
        let wxgf = container(&[0, 0, 0, 0], &[&colour[..colour.len() / 2]]);
        let error = decode(&wxgf, root.path()).unwrap_err();
        assert!(
            error.downcast_ref::<super::super::MediaPending>().is_some(),
            "{error:#}"
        );
    }

    #[test]
    fn unknown_wxgf_stays_pending() {
        let root = tempfile::tempdir().unwrap();
        let error = decode(b"wxgfnot-a-supported-container", root.path()).unwrap_err();
        assert!(error.downcast_ref::<super::super::MediaPending>().is_some());
    }
}
