use super::*;
use aes::cipher::{BlockEncrypt, KeyInit, generic_array::GenericArray};
use image::{DynamicImage, ImageFormat};
use std::io::Cursor;

fn sample_png() -> Vec<u8> {
    let mut out = Cursor::new(Vec::new());
    DynamicImage::new_rgb8(16, 12)
        .write_to(&mut out, ImageFormat::Png)
        .unwrap();
    out.into_inner()
}
fn store(root: &Path, name: &str, kind: &str, data: &[u8]) -> Value {
    fs::create_dir_all(root.join("media")).unwrap();
    let path = root.join("media").join(name);
    fs::write(&path, data).unwrap();
    json!({"name":name,"kind":kind,"path":path,"size":data.len(),"sha256":sha256(data)})
}
fn gif(width: u32) -> Vec<u8> {
    let mut data = Vec::new();
    {
        let mut encoder = image::codecs::gif::GifEncoder::new(&mut data);
        for color in [image::Rgba([255, 0, 0, 255]), image::Rgba([0, 255, 0, 255])] {
            encoder
                .encode_frame(image::Frame::from_parts(
                    image::RgbaImage::from_pixel(width, 2, color),
                    0,
                    0,
                    image::Delay::from_numer_denom_ms(100, 1),
                ))
                .unwrap();
        }
    }
    data
}
#[test]
fn outgoing_stages_exact_original_and_validates_reused_content() {
    let root = tempfile::tempdir().unwrap();
    let adapter = Media::new(root.path().join("adapter"));
    let media = store(root.path(), "原始图片.png", "image", &sample_png());
    let id = uuid::Uuid::new_v4().to_string();
    let staged = adapter.outgoing(root.path(), &id, 0, &media).unwrap();
    assert_eq!(staged.kind, "image");
    assert_eq!(staged.name, "原始图片.png");
    assert!(staged.path.ends_with(Path::new(&id).join("0/原始图片.png")));
    assert_eq!(fs::read(&staged.path).unwrap(), sample_png());
    assert_eq!(staged.md5, picture::md5(&sample_png()));
    fs::write(&staged.path, b"modified after staging").unwrap();
    assert!(adapter.outgoing(root.path(), &id, 0, &media).is_err());
}
#[test]
fn outgoing_gif_keeps_animation_and_conservatively_uses_file_for_native_limits() {
    let root = tempfile::tempdir().unwrap();
    let adapter = Media::new(root.path().join("adapter"));
    let id = uuid::Uuid::new_v4().to_string();
    let data = gif(16);
    let media = store(root.path(), "animation.gif", "gif", &data);
    let staged = adapter.outgoing(root.path(), &id, 0, &media).unwrap();
    assert_eq!(staged.kind, "gif");
    assert_eq!(fs::read(staged.path).unwrap(), data);
    let wide = gif(2001);
    let media = store(root.path(), "wide.gif", "image", &wide);
    let staged = adapter.outgoing(root.path(), &id, 1, &media).unwrap();
    assert_eq!(staged.kind, "file");
    assert!(staged.gif_attachment);
    let mut large = gif(16);
    assert_eq!(large.pop(), Some(0x3b));
    // A valid GIF comment extension makes an original larger than the native 1 MiB channel.
    large.extend_from_slice(&[0x21, 0xfe]);
    for _ in 0..4200 {
        large.push(255);
        large.extend_from_slice(&[b'x'; 255]);
    }
    large.extend_from_slice(&[0, 0x3b]);
    let media = store(root.path(), "large.gif", "gif", &large);
    let staged = adapter.outgoing(root.path(), &id, 2, &media).unwrap();
    assert_eq!(staged.kind, "file");
    assert!(staged.gif_attachment);
    assert_eq!(fs::read(staged.path).unwrap(), large);
}
#[test]
fn outgoing_path_name_size_hash_and_image_decode_fail_closed() {
    let root = tempfile::tempdir().unwrap();
    let adapter = Media::new(root.path().join("adapter"));
    let id = uuid::Uuid::new_v4().to_string();
    let media = store(root.path(), "note.txt", "file", b"offline content");
    for name in [
        "../escape",
        "CON.txt",
        "NUL",
        "LPT1.zip",
        "bad?.txt",
        "trailing. ",
    ] {
        let mut bad = media.clone();
        bad["name"] = json!(name);
        assert!(adapter.outgoing(root.path(), &id, 0, &bad).is_err());
    }
    let mut bad = media.clone();
    bad["sha256"] = json!("0".repeat(64));
    assert!(adapter.outgoing(root.path(), &id, 0, &bad).is_err());
    let mut bad = media.clone();
    bad["size"] = json!(1);
    assert!(adapter.outgoing(root.path(), &id, 0, &bad).is_err());
    let outside = root.path().join("outside.txt");
    fs::write(&outside, b"offline content").unwrap();
    let mut bad = media.clone();
    bad["path"] = json!(outside);
    assert!(adapter.outgoing(root.path(), &id, 0, &bad).is_err());
    let bad_image = store(root.path(), "bad.png", "image", b"not a picture");
    assert!(adapter.outgoing(root.path(), &id, 0, &bad_image).is_err());
    assert!(
        adapter
            .outgoing(root.path(), "not-an-id", 0, &media)
            .is_err()
    );
    assert!(adapter.outgoing(root.path(), &id, 12, &media).is_err());
}
#[test]
fn native_file_flags_use_actual_signatures() {
    let root = tempfile::tempdir().unwrap();
    let adapter = Media::new(root.path().join("adapter"));
    let id = uuid::Uuid::new_v4().to_string();
    let media = store(
        root.path(),
        "clip.mp4",
        "video",
        b"\0\0\0\x20ftypisomoffline",
    );
    let staged = adapter.outgoing(root.path(), &id, 0, &media).unwrap();
    assert_eq!(staged.kind, "file");
    assert!(staged.video_attachment);
    assert!(!staged.gif_attachment);
}
#[test]
fn staged_copy_md5_and_size_rechecked_before_commit() {
    let root = tempfile::tempdir().unwrap();
    let source = root.path().join("source");
    let target = root.path().join("target");
    fs::write(&source, b"verified bytes").unwrap();
    let digest = picture::md5(b"verified bytes");
    copy_verified(&source, &target, Some(14), &[digest.clone()]).unwrap();
    assert!(verified(&target, 14, &[digest]));
    let error = copy_verified(
        &source,
        &root.path().join("wrong"),
        Some(13),
        &["0".repeat(32)],
    )
    .unwrap_err();
    assert!(error.downcast_ref::<MediaPending>().is_some());
    assert!(!root.path().join("wrong").exists());
}
#[test]
fn local_metadata_and_ascii_utf16_image_keys_are_parsed_without_python() {
    let key = *b"0123456789ABCDEF";
    let cipher = aes::Aes128::new_from_slice(&key).unwrap();
    let source = sample_png();
    let mut probe = GenericArray::clone_from_slice(&source[..16]);
    cipher.encrypt_block(&mut probe);
    let mut ascii = b"\0prefix\0".to_vec();
    ascii.extend_from_slice(&key);
    ascii.push(0);
    assert_eq!(scan_image_key(&ascii, &probe), Some(key));
    let mut wide = vec![1];
    for byte in key {
        wide.extend_from_slice(&[byte, 0]);
    }
    wide.extend_from_slice(&[1, 1]);
    assert_eq!(scan_image_key(&wide, &probe), Some(key));
    let row = json!({"packed_info":b"prefix 0123456789abcdef0123456789abcdef suffix".to_vec(),"local_id":9,"sort_seq":12,"shard":"message__message_2.db"});
    assert_eq!(find_md5(&row).unwrap(), "0123456789abcdef0123456789abcdef");
    assert_eq!(
        message_key("test@chatroom", &row),
        "wx:test@chatroom:message__message_2.db:9:12"
    );
}
#[test]
fn cdn_allowlist_and_xml_entity_rules_are_preserved() {
    for url in [
        "https://mmbiz.qpic.cn/a?b=1",
        "http://vweixinf.tc.qq.com/gif",
        "https://wxapp.tc.qq.com:443/x",
    ] {
        assert!(emoji_uri(url).is_ok());
    }
    for url in [
        "file:///C:/private",
        "http://127.0.0.1/x",
        "https://mmbiz.qpic.cn.evil.test/x",
        "https://user@mmbiz.qpic.cn/x",
        "https://mmbiz.qpic.cn:444/x",
        "https://mmbiz.qpic.cn/x#fragment",
    ] {
        assert!(emoji_uri(url).is_err());
    }
    assert!(
        xml_document("<!DOCTYPE x [<!ENTITY leak SYSTEM 'file:///C:/private'>]><msg>&leak;</msg>")
            .is_err()
    );
    assert!(
        xml_document("sender:\n<msg><emoji md5='0123456789abcdef0123456789abcdef'/></msg>").is_ok()
    );
}

#[test]
fn inbound_local_picture_file_video_and_emoji_keep_verified_originals() {
    let root = tempfile::tempdir().unwrap();
    let account = root.path().join("account");
    fs::create_dir_all(&account).unwrap();
    let mut db = Db::media_fixture(account.clone(), "wxid_offline", None, Default::default());
    let mut media = Media::new(root.path().join("adapter"));
    let group = "123456@chatroom";
    let original = sample_png();
    let digest = picture::md5(&original);
    let folder = account
        .join("msg/attach")
        .join(picture::md5(group.as_bytes()))
        .join("2026-10/Img");
    fs::create_dir_all(&folder).unwrap();
    let encrypted: Vec<_> = original.iter().map(|byte| byte ^ 0x88).collect();
    fs::write(folder.join(format!("{digest}.dat")), encrypted).unwrap();
    let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":digest.as_bytes(),"content":""});
    let result = media.inbound(&mut db, None, group, &row, "image").unwrap();
    assert_eq!(result["kind"], "image");
    assert_eq!(
        fs::read(result["path"].as_str().unwrap()).unwrap(),
        original
    );
    let file_root = account.join("msg/file/2026-10");
    fs::create_dir_all(&file_root).unwrap();
    let file = b"verified original attachment";
    fs::write(file_root.join("renamed.bin"), file).unwrap();
    let row = json!({"local_id":2,"sort_seq":2,"shard":"offline","content":format!("<msg><appmsg><type>74</type><title>original.bin</title><appattach><totallen>{}</totallen><md5>{}</md5></appattach></appmsg></msg>",file.len(),picture::md5(file))});
    let result = media.inbound(&mut db, None, group, &row, "file").unwrap();
    assert_eq!(result["name"], "original.bin");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), file);
    let video = b"\0\0\0\x20ftypisomoffline_video_fixture";
    let video_root = account.join("msg/video/2026-10");
    fs::create_dir_all(&video_root).unwrap();
    fs::write(video_root.join("cached.mp4"), video).unwrap();
    let row = json!({"local_id":3,"sort_seq":3,"shard":"offline","content":format!("<msg><videomsg length='{}' md5='{}'/></msg>",video.len(),picture::md5(video))});
    let result = media.inbound(&mut db, None, group, &row, "video").unwrap();
    assert_eq!(result["kind"], "video");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), video);
    let gif = gif(16);
    let digest = picture::md5(&gif);
    let emoji_root = account.join("emoticon");
    fs::create_dir_all(&emoji_root).unwrap();
    fs::write(emoji_root.join(format!("{digest}.gif")), &gif).unwrap();
    let row = json!({"local_id":4,"sort_seq":4,"shard":"offline","content":format!("<msg><emoji md5='{digest}' len='{}'/></msg>",gif.len())});
    let result = media.inbound(&mut db, None, group, &row, "gif").unwrap();
    assert_eq!(result["kind"], "gif");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), gif);
}

#[test]
fn missing_and_partial_inbound_media_remain_retryable() {
    let root = tempfile::tempdir().unwrap();
    let account = root.path().join("account");
    fs::create_dir_all(&account).unwrap();
    let connection = rusqlite::Connection::open_in_memory().unwrap();
    connection.execute_batch("CREATE TABLE Name2Id(user_name TEXT);CREATE TABLE VoiceInfo(chat_name_id INTEGER,svr_id INTEGER,voice_data BLOB,create_time INTEGER);INSERT INTO Name2Id VALUES('123456@chatroom');").unwrap();
    connection
        .execute(
            "INSERT INTO VoiceInfo VALUES(1,7,?1,1)",
            [b"\x02#!SILK_V3\x20\x00partial".as_slice()],
        )
        .unwrap();
    let mut db = Db::media_fixture(
        account,
        "wxid_offline",
        None,
        [("message/media_0.db".to_owned(), connection)].into(),
    );
    let mut media = Media::new(root.path().join("adapter"));
    let group = "123456@chatroom";
    for (kind, row) in [
        (
            "image",
            json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":b"0123456789abcdef0123456789abcdef","content":""}),
        ),
        (
            "voice",
            json!({"local_id":2,"sort_seq":2,"server_id":7,"shard":"offline"}),
        ),
        (
            "file",
            json!({"local_id":3,"sort_seq":3,"shard":"offline","content":"<msg><appmsg><type>6</type><title>missing.txt</title><appattach><totallen>10</totallen><md5>0123456789abcdef0123456789abcdef</md5></appattach></appmsg></msg>"}),
        ),
    ] {
        let error = media.inbound(&mut db, None, group, &row, kind).unwrap_err();
        assert!(
            error.downcast_ref::<MediaPending>().is_some(),
            "{kind}: {error}"
        );
    }
    assert!(
        WalkDir::new(root.path().join("adapter"))
            .into_iter()
            .filter_map(|e| e.ok())
            .any(|e| e.file_name() == "voice.silk")
    );
    assert!(
        !WalkDir::new(root.path().join("adapter"))
            .into_iter()
            .filter_map(|e| e.ok())
            .any(|e| e.file_name() == "voice.wav")
    );
}
