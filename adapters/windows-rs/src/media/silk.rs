//! SILK SDK bindings. Packet parsing and atomic WAV output are owned by Rust.
use anyhow::{Result, bail, ensure};
use std::{
    cell::Cell,
    fs::{self, File},
    io::{Read, Seek, SeekFrom, Write},
    path::Path,
};

thread_local! {static CODEC_ASSERTION:Cell<bool>=const{Cell::new(false)};}
// pilk's SDK replaces the original assertion macro with this external callback.
// Keep it independent of Python and turn a codec invariant failure into a failed decode.
#[unsafe(no_mangle)]
extern "C" fn SKP_assert(condition: i32) {
    if condition == 0 {
        CODEC_ASSERTION.set(true);
    }
}

#[repr(C)]
#[derive(Default)]
struct DecControl {
    sample_rate: i32,
    frame_size: i32,
    frames_per_packet: i32,
    more_frames: i32,
    fec_offset: i32,
}

unsafe extern "C" {
    fn SKP_Silk_SDK_Get_Decoder_Size(size: *mut i32) -> i32;
    fn SKP_Silk_SDK_InitDecoder(state: *mut std::ffi::c_void) -> i32;
    fn SKP_Silk_SDK_Decode(
        state: *mut std::ffi::c_void,
        control: *mut DecControl,
        lost: i32,
        packet: *const u8,
        packet_size: i32,
        samples: *mut i16,
        sample_count: *mut i16,
    ) -> i32;
}

fn wav_header(bytes: u32) -> [u8; 44] {
    let mut out = [0; 44];
    out[0..4].copy_from_slice(b"RIFF");
    out[4..8].copy_from_slice(&(bytes + 36).to_le_bytes());
    out[8..16].copy_from_slice(b"WAVEfmt ");
    out[16..20].copy_from_slice(&16u32.to_le_bytes());
    out[20..22].copy_from_slice(&1u16.to_le_bytes());
    out[22..24].copy_from_slice(&1u16.to_le_bytes());
    out[24..28].copy_from_slice(&24000u32.to_le_bytes());
    out[28..32].copy_from_slice(&48000u32.to_le_bytes());
    out[32..34].copy_from_slice(&2u16.to_le_bytes());
    out[34..36].copy_from_slice(&16u16.to_le_bytes());
    out[36..40].copy_from_slice(b"data");
    out[40..44].copy_from_slice(&bytes.to_le_bytes());
    out
}

fn valid_wav(path: &Path) -> bool {
    let Ok(mut file) = File::open(path) else {
        return false;
    };
    let mut header = [0; 44];
    if file.read_exact(&mut header).is_err() {
        return false;
    }
    let bytes = u32::from_le_bytes(header[40..44].try_into().unwrap());
    bytes > 0
        && u64::from(bytes) <= super::MAX_FILE
        && bytes % 2 == 0
        && header == wav_header(bytes)
        && file
            .metadata()
            .is_ok_and(|m| m.len() == u64::from(bytes) + 44)
}

pub(super) fn decode(path: &Path) -> Result<std::path::PathBuf> {
    let wav = path.with_extension("wav");
    ensure!(
        !path.is_symlink() && !wav.is_symlink(),
        "语音缓存不能是符号链接"
    );
    if valid_wav(&wav) {
        return Ok(wav);
    }
    let temp = wav.with_extension("wav.part");
    ensure!(!temp.is_symlink(), "语音临时缓存不能是符号链接");
    let result = (|| {
        let mut source = File::open(path)?;
        let mut first = [0u8; 1];
        source.read_exact(&mut first)?;
        let header_len = 9;
        let mut header = vec![0; header_len];
        header[0] = first[0];
        if first[0] == 2 {
            source.read_exact(&mut header)?;
            ensure!(header == b"#!SILK_V3", "语音 SILK 文件头无效");
        } else {
            source.read_exact(&mut header[1..])?;
            ensure!(header == b"#!SILK_V3", "语音 SILK 文件头无效");
        }
        let mut state_size = 0;
        CODEC_ASSERTION.set(false);
        // The SDK stores aligned native integers in the opaque state.
        ensure!(
            unsafe { SKP_Silk_SDK_Get_Decoder_Size(&mut state_size) } == 0
                && (1..=1024 * 1024).contains(&state_size),
            "SILK 解码器初始化失败"
        );
        let mut state = vec![0u64; (state_size as usize).div_ceil(8)];
        let pointer = state.as_mut_ptr().cast();
        ensure!(
            unsafe { SKP_Silk_SDK_InitDecoder(pointer) } == 0,
            "SILK 解码器初始化失败"
        );
        let mut control = DecControl {
            sample_rate: 24000,
            frames_per_packet: 1,
            ..Default::default()
        };
        let mut output = File::create(&temp)?;
        output.write_all(&wav_header(0))?;
        let mut total = 0u32;
        let mut packet = [0u8; 5120];
        let mut samples = [0i16; 960];
        loop {
            let mut len = [0; 2];
            let n = source.read(&mut len[..1])?;
            if n == 0 {
                break;
            }
            source.read_exact(&mut len[1..])?;
            let length = i16::from_le_bytes(len);
            if length == -1 {
                ensure!(
                    source.read(&mut len[..1])? == 0,
                    "SILK 结束标记后存在未知数据"
                );
                break;
            }
            ensure!(
                length > 0 && length as usize <= packet.len(),
                "SILK 语音包长度无效"
            );
            source.read_exact(&mut packet[..length as usize])?;
            for frame in 0..5 {
                let mut count = samples.len() as i16;
                let result = unsafe {
                    SKP_Silk_SDK_Decode(
                        pointer,
                        &mut control,
                        0,
                        packet.as_ptr(),
                        i32::from(length),
                        samples.as_mut_ptr(),
                        &mut count,
                    )
                };
                ensure!(
                    result == 0
                        && !CODEC_ASSERTION.replace(false)
                        && count > 0
                        && count as usize <= samples.len(),
                    "SILK 语音包未完整解码"
                );
                total = total
                    .checked_add(count as u32 * 2)
                    .ok_or_else(|| anyhow::anyhow!("语音过大"))?;
                ensure!(
                    u64::from(total) <= super::MAX_FILE,
                    "语音解码结果超过 512 MiB"
                );
                for sample in &samples[..count as usize] {
                    output.write_all(&sample.to_le_bytes())?;
                }
                if control.more_frames == 0 {
                    break;
                }
                if frame == 4 {
                    bail!("SILK 语音包帧数无效");
                }
            }
        }
        ensure!(total > 0, "语音尚未完整解码");
        output.seek(SeekFrom::Start(0))?;
        output.write_all(&wav_header(total))?;
        output.sync_all()?;
        drop(output);
        super::commit(&temp, &wav)?;
        Ok(wav)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[repr(C)]
    struct EncControl {
        sample_rate: i32,
        max_rate: i32,
        packet_size: i32,
        bit_rate: i32,
        loss: i32,
        complexity: i32,
        fec: i32,
        dtx: i32,
    }
    unsafe extern "C" {
        fn SKP_Silk_SDK_Get_Encoder_Size(size: *mut i32) -> i32;
        fn SKP_Silk_SDK_InitEncoder(state: *mut std::ffi::c_void, control: *mut EncControl) -> i32;
        fn SKP_Silk_SDK_Encode(
            state: *mut std::ffi::c_void,
            control: *const EncControl,
            samples: *const i16,
            count: i32,
            packet: *mut u8,
            len: *mut i16,
        ) -> i32;
    }
    #[test]
    fn real_silk_roundtrip_and_cached_wav_preserve_original() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("voice.silk");
        let mut size = 0;
        assert_eq!(unsafe { SKP_Silk_SDK_Get_Encoder_Size(&mut size) }, 0);
        let mut state = vec![0u64; (size as usize).div_ceil(8)];
        let ptr = state.as_mut_ptr().cast();
        let mut control = EncControl {
            sample_rate: 24000,
            max_rate: 24000,
            packet_size: 480,
            bit_rate: 24000,
            loss: 0,
            complexity: 1,
            fec: 0,
            dtx: 0,
        };
        assert_eq!(unsafe { SKP_Silk_SDK_InitEncoder(ptr, &mut control) }, 0);
        control = EncControl {
            sample_rate: 24000,
            max_rate: 24000,
            packet_size: 480,
            bit_rate: 24000,
            loss: 0,
            complexity: 1,
            fec: 0,
            dtx: 0,
        };
        let mut original = b"\x02#!SILK_V3".to_vec();
        for frame in 0..50 {
            let samples: Vec<i16> = (0..480)
                .map(|i| {
                    ((f64::from(frame * 480 + i) * 440.0 * std::f64::consts::TAU / 24000.0).sin()
                        * 6000.0) as i16
                })
                .collect();
            let mut packet = [0; 5120];
            let mut n = packet.len() as i16;
            assert_eq!(
                unsafe {
                    SKP_Silk_SDK_Encode(
                        ptr,
                        &control,
                        samples.as_ptr(),
                        480,
                        packet.as_mut_ptr(),
                        &mut n,
                    )
                },
                0
            );
            assert!(n > 0);
            original.extend_from_slice(&n.to_le_bytes());
            original.extend_from_slice(&packet[..n as usize]);
        }
        fs::write(&path, &original).unwrap();
        let wav = decode(&path).unwrap();
        assert!(valid_wav(&wav));
        let data = fs::read(&wav).unwrap();
        assert_eq!(data.len(), 48000 + 44);
        assert!(data[44..].iter().any(|byte| *byte != 0));
        assert_eq!(fs::read(&path).unwrap(), original);
        assert_eq!(decode(&path).unwrap(), wav);
        let standard = root.path().join("standard.silk");
        fs::write(&standard, &original[1..]).unwrap();
        let standard_wav = decode(&standard).unwrap();
        assert!(valid_wav(&standard_wav));
        assert_eq!(fs::read(standard_wav).unwrap(), data);
        assert!(!path.with_extension("wav.part").exists());
    }
    #[test]
    fn truncated_silk_never_commits_partial_wav() {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("truncated.silk");
        fs::write(&path, b"\x02#!SILK_V3\x20\x00\x01\x02").unwrap();
        assert!(decode(&path).is_err());
        assert!(!path.with_extension("wav").exists());
        assert!(!path.with_extension("wav.part").exists());
    }
}
