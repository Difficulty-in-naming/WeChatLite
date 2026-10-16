use aes::{
    Aes128,
    cipher::{BlockDecrypt, KeyInit, generic_array::GenericArray},
};
use anyhow::{Context, Result, ensure};
use base64::Engine;
use image::{AnimationDecoder, DynamicImage, ImageFormat, ImageReader, Limits};
use md5::{Digest, Md5};
use std::{io::Cursor, path::Path};

pub(super) const V1: &[u8] = b"\x07\x08\x05\x56\x02\x05";
pub(super) const V2: &[u8] = b"\x07\x08V2\x08\x07";

pub(super) fn md5(data: &[u8]) -> String {
    hex::encode(Md5::digest(data))
}
pub(super) fn image_keys(cfg: u32, wxid: &str) -> ([u8; 16], u8) {
    let digest = md5(format!("{cfg}{wxid}").as_bytes());
    (digest.as_bytes()[..16].try_into().unwrap(), cfg as u8)
}
pub(super) fn looks_like_image(data: &[u8]) -> bool {
    data.starts_with(b"\xff\xd8\xff")
        || data.starts_with(b"\x89PNG\r\n\x1a\n")
        || data.starts_with(b"GIF8")
        || data.starts_with(b"RIFF")
        || data.starts_with(b"wxgf")
        || data.starts_with(b"BM")
}
pub(super) fn verifies_key(key: &[u8; 16], probe: &[u8]) -> bool {
    if probe.len() != 16 {
        return false;
    }
    let cipher = Aes128::new_from_slice(key).unwrap();
    let mut block = GenericArray::clone_from_slice(probe);
    cipher.decrypt_block(&mut block);
    looks_like_image(&block)
}
pub(super) fn ecb_unpad(data: &[u8], key: &[u8; 16]) -> Result<Vec<u8>> {
    ensure!(
        !data.is_empty() && data.len() % 16 == 0,
        "AES 数据尚未完整写入"
    );
    let cipher = Aes128::new_from_slice(key).unwrap();
    let mut plain = data.to_vec();
    for block in plain.chunks_exact_mut(16) {
        cipher.decrypt_block(GenericArray::from_mut_slice(block));
    }
    let pad = usize::from(*plain.last().unwrap());
    ensure!(
        pad > 0
            && pad <= 16
            && plain[plain.len() - pad..]
                .iter()
                .all(|b| usize::from(*b) == pad),
        "AES 数据填充或密钥无效"
    );
    plain.truncate(plain.len() - pad);
    Ok(plain)
}
pub(super) fn strip_footer(data: &mut Vec<u8>) {
    for (start, end) in [
        (&b"\xff\xd8"[..], &b"\xff\xd9"[..]),
        (&b"\x89PNG\r\n\x1a\n"[..], &b"IEND\xaeB`\x82"[..]),
    ] {
        if !data.starts_with(start) || data.ends_with(end) {
            continue;
        }
        if let Some(pos) = data.windows(end.len()).rposition(|part| part == end) {
            let cut = pos + end.len();
            if data.len() - cut <= 32 {
                data.truncate(cut);
            }
        }
    }
}
pub(super) fn decrypt(data: &[u8], aes_key: Option<&[u8; 16]>, xor: u8) -> Result<Vec<u8>> {
    ensure!(!data.is_empty(), "微信图片缓存为空");
    let mut plain = if data.starts_with(V2) {
        ensure!(data.len() >= 15, "微信图片缓存尚未完整写入");
        let aes_len = u32::from_le_bytes(data[6..10].try_into().unwrap()) as usize;
        let xor_len = u32::from_le_bytes(data[10..14].try_into().unwrap()) as usize;
        let aes_block = aes_len
            .checked_div(16)
            .and_then(|n| n.checked_add(1))
            .and_then(|n| n.checked_mul(16))
            .context("微信图片 AES 长度无效")?;
        let aes_end = 15usize
            .checked_add(aes_block)
            .context("微信图片 AES 长度无效")?;
        let xor_start = data
            .len()
            .checked_sub(xor_len)
            .context("微信图片 XOR 长度无效")?;
        ensure!(aes_end <= xor_start, "微信图片缓存尚未完整写入");
        let key = aes_key.context("微信图片解密密钥未加载，请在微信点击任意图片查看大图后重试")?;
        let mut out = ecb_unpad(&data[15..aes_end], key)?;
        ensure!(out.len() == aes_len, "微信图片 AES 明文长度无效");
        out.extend_from_slice(&data[aes_end..xor_start]);
        out.extend(data[xor_start..].iter().map(|byte| byte ^ xor));
        out
    } else if data.starts_with(V1) {
        ensure!(data.len() > 22, "微信图片缓存尚未完整写入");
        data[22..].iter().map(|byte| byte ^ xor).collect()
    } else if looks_like_image(data) {
        data.to_vec()
    } else {
        let key = [xor, 0x88, 0x30, 0xff, 0xe9]
            .into_iter()
            .find(|key| {
                let head: Vec<_> = data.iter().take(16).map(|byte| byte ^ key).collect();
                looks_like_image(&head)
            })
            .context("无法识别的微信图片加密格式")?;
        data.iter().map(|byte| byte ^ key).collect()
    };
    strip_footer(&mut plain);
    ensure!(looks_like_image(&plain), "微信图片解密结果无效");
    Ok(plain)
}

pub(super) fn format(data: &[u8]) -> Result<ImageFormat> {
    let format = image::guess_format(data).context("图片格式不支持或缓存不完整")?;
    ensure!(
        matches!(
            format,
            ImageFormat::Jpeg
                | ImageFormat::Png
                | ImageFormat::Gif
                | ImageFormat::WebP
                | ImageFormat::Bmp
        ),
        "图片格式暂不支持，原消息已保留"
    );
    Ok(format)
}
pub(super) fn extension(format: ImageFormat) -> &'static str {
    match format {
        ImageFormat::Jpeg => "jpg",
        ImageFormat::Png => "png",
        ImageFormat::Gif => "gif",
        ImageFormat::WebP => "webp",
        ImageFormat::Bmp => "bmp",
        _ => "img",
    }
}
fn limits(max: u32) -> Limits {
    let mut result = Limits::default();
    result.max_image_width = Some(max);
    result.max_image_height = Some(max);
    result.max_alloc = Some(512 * 1024 * 1024);
    result
}
fn complete(data: &[u8], format: ImageFormat) -> bool {
    match format {
        ImageFormat::Jpeg => data.ends_with(b"\xff\xd9"),
        ImageFormat::Png => data.ends_with(b"IEND\xaeB`\x82"),
        ImageFormat::Gif => data.last() == Some(&0x3b),
        ImageFormat::WebP => {
            data.len() >= 12
                && u32::from_le_bytes(data[4..8].try_into().unwrap()) as usize + 8 == data.len()
        }
        _ => true,
    }
}
pub(super) fn validate(data: &[u8], max: u32) -> Result<(ImageFormat, u32, u32)> {
    let format = format(data)?;
    ensure!(complete(data, format), "图片缓存尚未完整写入");
    let mut reader = ImageReader::with_format(Cursor::new(data), format);
    reader.limits(limits(max));
    let (width, height) = reader.into_dimensions()?;
    ensure!(
        width > 0 && height > 0 && width <= max && height <= max,
        "图片分辨率超过允许范围"
    );
    match format {
        ImageFormat::Gif => {
            let mut decoder = image::codecs::gif::GifDecoder::new(Cursor::new(data))?;
            use image::ImageDecoder;
            decoder.set_limits(limits(max))?;
            let mut frames = 0;
            for frame in decoder.into_frames() {
                frame.context("GIF 未完整解码")?;
                frames += 1;
                ensure!(frames <= 10000, "GIF 帧数超过安全解码范围");
            }
            ensure!(frames > 0, "GIF 没有可解码的图像");
        }
        ImageFormat::WebP => {
            let mut decoder = image::codecs::webp::WebPDecoder::new(Cursor::new(data))?;
            use image::ImageDecoder;
            decoder.set_limits(limits(max))?;
            if decoder.has_animation() {
                for frame in decoder.into_frames() {
                    frame?;
                }
            } else {
                DynamicImage::from_decoder(decoder)?;
            }
        }
        _ => {
            let mut reader = ImageReader::with_format(Cursor::new(data), format);
            reader.limits(limits(max));
            reader.decode()?;
        }
    }
    Ok((format, width, height))
}
pub fn avatar_data(data: &[u8]) -> String {
    (|| -> Result<String> {
        ensure!(
            !data.is_empty() && data.len() <= 2 * 1024 * 1024,
            "头像缓存大小无效"
        );
        let reader = ImageReader::new(Cursor::new(data)).with_guessed_format()?;
        let (w, h) = reader.into_dimensions()?;
        ensure!(w > 0 && h > 0 && w <= 2048 && h <= 2048, "头像像素范围无效");
        let mut reader = ImageReader::new(Cursor::new(data)).with_guessed_format()?;
        reader.limits(limits(2048));
        let picture = reader.decode()?.thumbnail(64, 64).into_rgba8();
        let mut rgb = image::RgbImage::new(picture.width(), picture.height());
        for (x, y, pixel) in picture.enumerate_pixels() {
            let alpha = u32::from(pixel[3]);
            let color = std::array::from_fn(|i| {
                ((u32::from(pixel[i]) * alpha + 255 * (255 - alpha) + 127) / 255) as u8
            });
            rgb.put_pixel(x, y, image::Rgb(color));
        }
        let mut jpeg = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, 82).encode_image(&rgb)?;
        Ok(base64::engine::general_purpose::STANDARD.encode(jpeg))
    })()
    .unwrap_or_default()
}
pub(super) fn read_and_validate(path: &Path, max: u32) -> Result<(ImageFormat, u32, u32)> {
    let data = super::read_limited(path, super::MAX_FILE)?;
    validate(&data, max)
}

#[cfg(test)]
mod tests {
    use super::*;
    use aes::cipher::BlockEncrypt;
    fn png() -> Vec<u8> {
        let mut out = Cursor::new(Vec::new());
        DynamicImage::new_rgb8(8, 6)
            .write_to(&mut out, ImageFormat::Png)
            .unwrap();
        out.into_inner()
    }
    #[test]
    fn all_legacy_xors_v1_and_v2_preserve_exact_picture() {
        let source = png();
        let (key, xor) = image_keys(123456, "wxid_offline");
        for mask in [xor, 0x88, 0x30, 0xff, 0xe9] {
            let encrypted: Vec<_> = source.iter().map(|b| b ^ mask).collect();
            assert_eq!(decrypt(&encrypted, Some(&key), xor).unwrap(), source);
        }
        let mut v1 = V1.to_vec();
        v1.extend_from_slice(&[0; 16]);
        v1.extend(source.iter().map(|b| b ^ xor));
        assert_eq!(decrypt(&v1, None, xor).unwrap(), source);
        let aes_len = 32;
        let xor_len = 10;
        let mut encrypted = source[..aes_len].to_vec();
        encrypted.extend_from_slice(&[16; 16]);
        let cipher = Aes128::new_from_slice(&key).unwrap();
        for block in encrypted.chunks_exact_mut(16) {
            cipher.encrypt_block(GenericArray::from_mut_slice(block));
        }
        let mut v2 = V2.to_vec();
        v2.extend_from_slice(&(aes_len as u32).to_le_bytes());
        v2.extend_from_slice(&(xor_len as u32).to_le_bytes());
        v2.push(0);
        v2.extend_from_slice(&encrypted);
        v2.extend_from_slice(&source[aes_len..source.len() - xor_len]);
        v2.extend(source[source.len() - xor_len..].iter().map(|b| b ^ xor));
        assert!(verifies_key(&key, &v2[15..31]));
        assert_eq!(decrypt(&v2, Some(&key), xor).unwrap(), source);
        assert!(decrypt(&v2[..20], Some(&key), xor).is_err());
        assert!(decrypt(&v2, None, xor).is_err());
        let wrong = [0u8; 16];
        assert!(decrypt(&v2, Some(&wrong), xor).is_err());
    }
    #[test]
    fn image_validation_rejects_truncation_and_oversized_pixels() {
        let source = png();
        assert_eq!(validate(&source, 12000).unwrap().1, 8);
        assert!(validate(&source[..source.len() - 4], 12000).is_err());
        assert!(validate(&source, 4).is_err());
        let mut footer = source.clone();
        footer.extend_from_slice(&[7; 24]);
        strip_footer(&mut footer);
        assert_eq!(footer, source);
    }
    #[test]
    fn avatar_is_bounded_jpeg_and_invalid_data_is_optional() {
        let mut out = Cursor::new(Vec::new());
        DynamicImage::new_rgba8(128, 64)
            .write_to(&mut out, ImageFormat::Png)
            .unwrap();
        let encoded = avatar_data(&out.into_inner());
        let decoded = base64::engine::general_purpose::STANDARD
            .decode(encoded)
            .unwrap();
        let picture = image::load_from_memory(&decoded).unwrap();
        assert_eq!((picture.width(), picture.height()), (64, 32));
        assert_eq!(picture.into_rgb8().get_pixel(0, 0).0, [255; 3]);
        assert!(avatar_data(b"bad").is_empty());
        assert!(avatar_data(&vec![0; 2 * 1024 * 1024 + 1]).is_empty());
    }
}
