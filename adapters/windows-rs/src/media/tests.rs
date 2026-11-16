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

#[test]
fn reencoded_record_images_require_the_complete_same_request_cdn_receipt() {
    let temp = tempfile::tempdir().unwrap();
    let bytes = sample_png();
    let path = temp.path().join("download.part");
    fs::write(&path, &bytes).unwrap();
    let mut resource = DownloadResource {
        kind: "image",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: 99,
        md5: "c".repeat(32),
    };
    let id = download_id("g", "record-image", &resource);
    let receipt = json!({"ret":0,"status":"complete","request_id":id,"path":path,"size":bytes.len(),"md5":picture::md5(&bytes)});
    assert!(completed_download(&receipt, temp.path(), &path, &id, &resource, false).is_err());
    assert_eq!(
        fs::read(
            completed_download(&receipt, temp.path(), &path, &id, &resource, true)
                .unwrap()
                .unwrap()
        )
        .unwrap(),
        bytes
    );
    for (field, value) in [
        ("ret", json!(-1)),
        ("request_id", json!("unrelated")),
        ("md5", json!("d".repeat(32))),
        ("size", json!(bytes.len() + 1)),
        ("path", json!(temp.path().join("other.part"))),
    ] {
        let mut bad = receipt.clone();
        bad[field] = value;
        assert!(completed_download(&bad, temp.path(), &path, &id, &resource, true).is_err());
    }
    let mut unfinished = receipt.clone();
    unfinished["status"] = json!("downloading");
    assert!(
        completed_download(&unfinished, temp.path(), &path, &id, &resource, true)
            .unwrap()
            .is_none()
    );
    resource.kind = "file";
    assert!(completed_download(&receipt, temp.path(), &path, &id, &resource, true).is_err());
    resource.kind = "image";
    fs::write(&path, b"tampered").unwrap();
    assert!(completed_download(&receipt, temp.path(), &path, &id, &resource, true).is_err());
}

#[test]
fn completed_picture_receipt_accepts_canonical_alias_only_for_the_expected_private_file() {
    let temp = tempfile::tempdir().unwrap();
    let folder = temp.path().join("downloads");
    fs::create_dir(&folder).unwrap();
    let partial = folder.join("download.part");
    let bytes = sample_png();
    fs::write(&partial, &bytes).unwrap();
    let resource = DownloadResource {
        kind: "image",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: 99,
        md5: "c".repeat(32),
    };
    let id = download_id("g", "alias-image", &resource);
    // The existing parent component resolves to the same file, but cannot pass
    // the lexical comparison. This exercises the canonical fallback on every OS.
    let alias = folder.join("../downloads/download.part");
    assert_ne!(absolute_comparable(&alias), absolute_comparable(&partial));
    let receipt = json!({"ret":0,"status":"complete","request_id":id,
        "path":alias,"size":bytes.len(),"md5":picture::md5(&bytes)});
    assert_eq!(
        completed_download(&receipt, &folder, &partial, &id, &resource, true)
            .unwrap()
            .unwrap(),
        partial.canonicalize().unwrap(),
    );
    // Identical bytes alone cannot authorize another path, even inside the cache.
    for other in [folder.join("other.part"), temp.path().join("outside.part")] {
        fs::write(&other, &bytes).unwrap();
        let mut receipt = receipt.clone();
        receipt["path"] = json!(other);
        assert!(completed_download(&receipt, &folder, &partial, &id, &resource, true).is_err());
    }
    assert_eq!(fs::read(&partial).unwrap(), bytes);
}

#[test]
fn record_attachment_cache_is_bound_to_resource_and_detects_modified_bytes() {
    let temp = tempfile::tempdir().unwrap();
    let mut media = Media::new(temp.path().join("adapter"));
    let bytes = sample_png();
    let mut item = crate::records::Attachment {
        kind: "image",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: bytes.len() as u64,
        md5: picture::md5(&bytes),
        name: "photo.png".into(),
    };
    let resource = DownloadResource {
        kind: "image",
        file_id: item.file_id.clone(),
        aes: item.aes.clone(),
        length: item.length,
        md5: item.md5.clone(),
    };
    let folder = media.directory.join("feishu/media/wechat-download");
    fs::create_dir_all(&folder).unwrap();
    let partial = folder.join(format!("{}.part", download_id("g", "record-1", &resource)));
    fs::write(&partial, &bytes).unwrap();
    let first = media.record_attachment("g", "record-1", &item).unwrap();
    fs::remove_file(&partial).unwrap();
    assert_eq!(
        media.record_attachment("g", "record-1", &item).unwrap(),
        first
    );
    item.file_id = "c".repeat(194);
    assert!(media.record_attachment("g", "record-1", &item).is_err());
    item.file_id = resource.file_id;
    fs::write(first["path"].as_str().unwrap(), b"modified").unwrap();
    assert!(media.record_attachment("g", "record-1", &item).is_err());
}

#[test]
fn inbound_downloaded_originals_do_not_require_a_wechat_cache_or_image_key() {
    let root = tempfile::tempdir().unwrap();
    let mut db = Db::media_fixture(
        root.path().join("account"),
        "wxid_offline",
        None,
        Default::default(),
    );
    let group = "123456@chatroom";
    let mut media = Media::new(root.path().join("adapter"));
    for (kind, bytes) in [
        ("image", sample_png()),
        ("file", b"complete software attachment fixture".to_vec()),
    ] {
        let digest = picture::md5(&bytes);
        let resource = DownloadResource {
            kind,
            file_id: "a".repeat(194),
            aes: "b".repeat(32),
            length: bytes.len() as u64,
            md5: digest.clone(),
        };
        let xml = if kind == "image" {
            format!(
                "<msg><img cdnbigimgurl='{}' aeskey='{}' hdlength='{}' md5='{digest}'/></msg>",
                resource.file_id,
                resource.aes,
                bytes.len()
            )
        } else {
            format!(
                "<msg><appmsg><type>6</type><title>software.exe</title><md5>{digest}</md5><appattach><totallen>{}</totallen><cdnattachurl>{}</cdnattachurl><aeskey>{}</aeskey></appattach></appmsg></msg>",
                bytes.len(),
                resource.file_id,
                resource.aes
            )
        };
        let row =
            json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":[],"content":xml});
        let key = message_key(group, &row);
        let folder = media.directory.join("feishu/media/wechat-download");
        fs::create_dir_all(&folder).unwrap();
        fs::write(
            folder.join(format!("{}.part", download_id(group, &key, &resource))),
            &bytes,
        )
        .unwrap();
        let actual = media.inbound(&mut db, group, &row, kind).unwrap();
        assert_eq!(fs::read(actual["path"].as_str().unwrap()).unwrap(), bytes);
        assert!(media.image_key.is_none() && !media.downloads.is_alive());
    }
}

#[test]
fn received_standard_picture_downloads_without_a_local_wechat_picture_cache() {
    let root = tempfile::tempdir().unwrap();
    let mut db = Db::media_fixture(
        root.path().join("account"),
        "wxid_offline_self",
        None,
        Default::default(),
    );
    let mut media = Media::new(root.path().join("adapter"));
    let bytes = sample_png();
    let digest = picture::md5(&bytes);
    let resource = DownloadResource {
        kind: "image_mid",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: bytes.len() as u64,
        md5: digest.clone(),
    };
    let group = "123456@chatroom";
    let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":[],
        "sender_id":7,"sender_username":"wxid_other_member",
        "content":format!("<msg><img cdnbigimgurl='' hdlength='0' cdnmidimgurl='{}' aeskey='{}' length='{}' md5='{digest}'/></msg>",
            resource.file_id, resource.aes, bytes.len())});
    // Seed only the adapter-owned CDN result, never WeChat's local picture cache.
    let folder = media.directory.join("feishu/media/wechat-download");
    fs::create_dir_all(&folder).unwrap();
    let partial = folder.join(format!(
        "{}.part",
        download_id(group, &message_key(group, &row), &resource)
    ));
    fs::write(&partial, &bytes).unwrap();
    let actual = media.inbound(&mut db, group, &row, "image").unwrap();
    assert_eq!(actual["kind"], "image");
    assert_eq!(fs::read(actual["path"].as_str().unwrap()).unwrap(), bytes);
    assert!(!db.account_dir.join("msg/attach").exists());
    assert!(media.image_key.is_none() && !media.downloads.is_alive());

    // Strict mode still requires the declared resource size and digest.
    let receipt = json!({"ret":0,"status":"complete","request_id":download_id(group,"fixture",&resource),
        "path":partial,"size":bytes.len(),"md5":digest});
    assert!(
        completed_download(
            &receipt,
            &folder,
            &partial,
            &download_id(group, "fixture", &resource),
            &resource,
            false
        )
        .unwrap()
        .is_some()
    );
    let mut changed = DownloadResource {
        length: resource.length + 1,
        ..resource
    };
    let id = download_id(group, "fixture", &changed);
    let mut wrong_size = receipt.clone();
    wrong_size["request_id"] = json!(id);
    assert!(completed_download(&wrong_size, &folder, &partial, &id, &changed, false).is_err());
    changed.length -= 1;
    changed.md5 = "c".repeat(32);
    let id = download_id(group, "fixture", &changed);
    let mut wrong_digest = receipt;
    wrong_digest["request_id"] = json!(id);
    assert!(completed_download(&wrong_digest, &folder, &partial, &id, &changed, false).is_err());
}

#[test]
fn ordinary_reencoded_pictures_require_a_complete_resource_bound_receipt_and_decodable_bytes() {
    for kind in ["image", "image_mid"] {
        for failure in [
            "",
            "no-receipt",
            "unfinished",
            "foreign-request",
            "wrong-checksum",
            "invalid-image",
            "footer-png",
            "footer-jpeg",
            "truncated-image",
            "excess-footer",
            "corrupt-body-with-footer",
        ] {
            let root = tempfile::tempdir().unwrap();
            let mut db = Db::media_fixture(
                root.path().join("account"),
                "fixture-self",
                None,
                Default::default(),
            );
            let mut media = Media::new(root.path().join("adapter"));
            let group = "123456@chatroom";
            let resource = DownloadResource {
                kind,
                file_id: "a".repeat(194),
                aes: "b".repeat(32),
                length: 999,
                md5: "c".repeat(32),
            };
            let (url, length) = if kind == "image" {
                ("cdnbigimgurl", "hdlength")
            } else {
                ("cdnmidimgurl", "length")
            };
            let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":[],
                "content":format!("<msg><img {url}='{}' {length}='999' aeskey='{}' md5='{}'/></msg>",resource.file_id,resource.aes,resource.md5)});
            let id = download_id(group, &message_key(group, &row), &resource);
            let folder = media.directory.join("feishu/media/wechat-download");
            fs::create_dir_all(&folder).unwrap();
            let partial = folder.join(format!("{id}.part"));
            let mut original = sample_png();
            if failure == "footer-jpeg" {
                let mut out = Cursor::new(Vec::new());
                DynamicImage::new_rgb8(16, 12)
                    .write_to(&mut out, ImageFormat::Jpeg)
                    .unwrap();
                original = out.into_inner();
            }
            let mut bytes = if failure == "invalid-image" {
                b"not a decodable picture".to_vec()
            } else {
                original.clone()
            };
            if failure == "truncated-image" {
                bytes.truncate(bytes.len() - 4);
            }
            if failure == "corrupt-body-with-footer" {
                let body = bytes.windows(4).position(|part| part == b"IDAT").unwrap() + 4;
                bytes[body] ^= 0xff;
            }
            let footer = match failure {
                "footer-png" => 24,
                "footer-jpeg" => 32,
                "excess-footer" => 33,
                "truncated-image" => 16,
                "corrupt-body-with-footer" => 24,
                _ => 0,
            };
            bytes.extend(std::iter::repeat_n(7, footer));
            fs::write(&partial, &bytes).unwrap();
            let mut receipt = json!({"ret":0,"status":"complete","request_id":id,"path":partial,"size":bytes.len(),"md5":picture::md5(&bytes)});
            if failure == "unfinished" {
                receipt["status"] = json!("downloading");
            }
            if failure == "foreign-request" {
                receipt["request_id"] = json!("unrelated");
            }
            if failure == "wrong-checksum" {
                receipt["md5"] = json!("d".repeat(32));
            }
            if failure != "no-receipt" {
                fs::write(
                    folder.join(format!("{id}.complete.json")),
                    serde_json::to_vec(&receipt).unwrap(),
                )
                .unwrap();
            }
            let result = media.inbound(&mut db, group, &row, "image");
            if matches!(failure, "" | "footer-png" | "footer-jpeg") {
                let full = result.unwrap();
                assert_eq!(full["preview"], false);
                assert_eq!(
                    full["name"],
                    if failure == "footer-jpeg" {
                        "image.jpg"
                    } else {
                        "image.png"
                    }
                );
                assert_eq!(fs::read(full["path"].as_str().unwrap()).unwrap(), original);
                // Normalize only the delivered copy, after receipt verification.
                assert_eq!(fs::read(&partial).unwrap(), bytes);
            } else {
                let error = result.unwrap_err();
                assert!(error.is::<MediaPending>(), "{kind}: {failure}");
                if matches!(
                    failure,
                    "truncated-image" | "excess-footer" | "corrupt-body-with-footer"
                ) {
                    let diagnostic = error.to_string();
                    assert!(diagnostic.contains("已校验的微信 CDN 图片无法完整解码"));
                    assert!(diagnostic.contains("format=png bytes="));
                    assert!(!diagnostic.contains(&resource.aes));
                    assert!(!diagnostic.contains(&resource.file_id));
                    if failure == "excess-footer" {
                        assert!(diagnostic.contains("end-marker-trailing=33"));
                    } else if failure == "truncated-image" {
                        assert!(diagnostic.contains("end-marker=missing"));
                    }
                }
            }
            assert!(!media.downloads.is_alive());
            assert!(
                !media
                    .cache_dir(&message_key(group, &row))
                    .unwrap()
                    .join("placeholder.png")
                    .exists()
            );
        }
    }
}

#[test]
fn record_picture_footer_is_normalized_after_verifying_the_original_download() {
    let root = tempfile::tempdir().unwrap();
    let mut media = Media::new(root.path().join("adapter"));
    let original = sample_png();
    let mut bytes = original.clone();
    bytes.extend_from_slice(&[7; 24]);
    let item = crate::records::Attachment {
        kind: "image",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: original.len() as u64,
        md5: picture::md5(&original),
        name: "photo.png".into(),
    };
    let resource = DownloadResource {
        kind: item.kind,
        file_id: item.file_id.clone(),
        aes: item.aes.clone(),
        length: item.length,
        md5: item.md5.clone(),
    };
    let id = download_id("g", "record-footer", &resource);
    let folder = media.directory.join("feishu/media/wechat-download");
    fs::create_dir_all(&folder).unwrap();
    let partial = folder.join(format!("{id}.part"));
    fs::write(&partial, &bytes).unwrap();
    // The message describes the unpadded image. Only the same-request receipt
    // can authorize these different download bytes before normalization.
    assert!(media.record_attachment("g", "record-footer", &item).is_err());
    let receipt = json!({"ret":0,"status":"complete","request_id":id,"path":partial,
        "size":bytes.len(),"md5":picture::md5(&bytes)});
    fs::write(
        folder.join(format!("{id}.complete.json")),
        serde_json::to_vec(&receipt).unwrap(),
    )
    .unwrap();
    let full = media.record_attachment("g", "record-footer", &item).unwrap();
    assert_eq!(fs::read(full["path"].as_str().unwrap()).unwrap(), original);
    assert_eq!(fs::read(&partial).unwrap(), bytes);
    assert_eq!(
        media.record_attachment("g", "record-footer", &item).unwrap(),
        full
    );
    assert!(!media.downloads.is_alive());
}

#[test]
fn corrupt_local_picture_and_legacy_blank_cache_remain_pending_instead_of_delivering_gray() {
    let root = tempfile::tempdir().unwrap();
    let mut db = Db::media_fixture(
        root.path().join("account"),
        "fixture-self",
        None,
        Default::default(),
    );
    let mut media = Media::new(root.path().join("adapter"));
    let group = "123456@chatroom";
    let digest = "d".repeat(32);
    let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":[],"content":format!("<msg><img md5='{digest}'/></msg>")});
    let folder = db
        .account_dir
        .join("msg/attach")
        .join(picture::md5(group.as_bytes()));
    fs::create_dir_all(&folder).unwrap();
    fs::write(folder.join(format!("{digest}.dat")), b"corrupt picture").unwrap();
    let cache = media.cache_dir(&message_key(group, &row)).unwrap();
    fs::write(cache.join("image.png"), PLACEHOLDER_PNG).unwrap();
    assert!(
        media
            .inbound(&mut db, group, &row, "image")
            .unwrap_err()
            .is::<MediaPending>()
    );
    assert!(!cache.join("placeholder.png").exists());
    assert_eq!(PLACEHOLDER_PNG.len(), 691);
    assert_eq!(
        sha256(PLACEHOLDER_PNG),
        "ea1e8edd73b84213a88451e1cbe5eb084851a656b4879d749ac2c56a5ab72760"
    );
}

#[test]
fn thumbnail_only_picture_is_delivered_then_upgraded_without_caching_it_as_full() {
    let root = tempfile::tempdir().unwrap();
    let account = root.path().join("account");
    let mut db = Db::media_fixture(account.clone(), "wxid_offline", None, Default::default());
    let mut media = Media::new(root.path().join("adapter"));
    let group = "123456@chatroom";
    let bytes = sample_png();
    assert!(
        bytes.len() < 512,
        "small thumbnails must not be filtered by file size"
    );
    let digest = picture::md5(&bytes);
    let folder = account
        .join("msg/attach")
        .join(picture::md5(group.as_bytes()))
        .join("2026-11/Img");
    fs::create_dir_all(&folder).unwrap();
    let thumbnail = folder.join(format!("{digest}_t.dat"));
    fs::write(
        &thumbnail,
        bytes.iter().map(|b| b ^ 0x88).collect::<Vec<_>>(),
    )
    .unwrap();
    let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":[],
        "content":format!("<msg><img cdnbigimgurl='{}' aeskey='{}' md5='{digest}'/></msg>", "a".repeat(194), "b".repeat(32))});
    let preview = media.inbound(&mut db, group, &row, "image").unwrap();
    assert_eq!(preview["kind"], "image");
    assert_eq!(preview["preview"], true);
    assert_eq!(preview["name"], "preview.png");
    assert_eq!(fs::read(preview["path"].as_str().unwrap()).unwrap(), bytes);
    assert!(cached_picture(&media.cache_dir(&message_key(group, &row)).unwrap()).is_none());
    assert!(media.image_key.is_none() && !media.downloads.is_alive());

    // Restarting and the source thumbnail disappearing must not erase the preview.
    fs::rename(&thumbnail, root.path().join("retained-thumbnail.dat")).unwrap();
    let mut media = Media::new(root.path().join("adapter"));
    assert_eq!(
        media.inbound(&mut db, group, &row, "image").unwrap(),
        preview
    );
    // The complete picture then becomes available and must replace the preview.
    fs::write(folder.join(format!("{digest}.dat")), &bytes).unwrap();
    let complete = media.inbound(&mut db, group, &row, "image").unwrap();
    assert_eq!(complete["preview"], false);
    assert_eq!(complete["name"], "image.png");
    assert_eq!(fs::read(complete["path"].as_str().unwrap()).unwrap(), bytes);
}

#[test]
fn thumbnail_fallback_survives_a_corrupt_full_image_but_rejects_bad_or_unrelated_previews() {
    let root = tempfile::tempdir().unwrap();
    let account = root.path().join("account");
    let mut db = Db::media_fixture(account.clone(), "wxid_offline", None, Default::default());
    let group = "123456@chatroom";
    let bytes = sample_png();
    let digest = picture::md5(&bytes);
    let folder = account
        .join("msg/attach")
        .join(picture::md5(group.as_bytes()))
        .join("2026-11/Img");
    fs::create_dir_all(&folder).unwrap();
    let row = json!({"local_id":1,"sort_seq":1,"shard":"offline","packed_info":digest.as_bytes(),
        "content":format!("<msg><img md5='{digest}'/></msg>")});
    fs::write(folder.join(format!("{digest}.dat")), b"corrupt full image").unwrap();
    fs::write(folder.join(format!("{digest}_t.dat")), &bytes).unwrap();
    let mut media = Media::new(root.path().join("adapter-valid"));
    assert_eq!(
        media.inbound(&mut db, group, &row, "image").unwrap()["preview"],
        true
    );

    // A different message's valid thumbnail cannot be substituted.
    let missing = "d".repeat(32);
    let mut other = row;
    other["packed_info"] = json!(missing.as_bytes());
    other["content"] = json!(format!("<msg><img md5='{missing}'/></msg>"));
    fs::write(
        folder.join(format!("{missing}_t.dat")),
        &bytes[..bytes.len() / 2],
    )
    .unwrap();
    let mut media = Media::new(root.path().join("adapter-invalid"));
    assert!(
        media
            .inbound(&mut db, group, &other, "image")
            .unwrap_err()
            .is::<MediaPending>()
    );
    assert!(media.image_key.is_none() && !media.downloads.is_alive());
}

#[test]
fn picture_parameter_diagnostics_do_not_expose_xml_keys_or_resource_ids() {
    let secret = "b".repeat(32);
    let resource = "a".repeat(194);
    let xml = format!(
        "<msg><img cdnbigimgurl='{resource}' aeskey='{secret}' md5='{}' hdlength='0'/></msg>",
        "c".repeat(32)
    );
    let message = format!("{:#}", image_resource(&xml).err().unwrap());
    assert!(message.contains("原图：资源编号有效=true 大小有效=false AES有效=true MD5有效=true"));
    for private in [&secret, &resource, &"c".repeat(32)] {
        assert!(!message.contains(private));
    }
    let malformed = format!("<msg {secret}");
    assert!(!format!("{:#}", image_resource(&malformed).err().unwrap()).contains(&secret));
}

#[test]
fn picture_resources_keep_original_and_standard_fields_paired_and_never_use_thumbnails() {
    let original = "a".repeat(194);
    let standard = "b".repeat(194);
    let aes = "c".repeat(32);
    let digest = "d".repeat(32);
    let xml = format!(
        "<msg><img cdnbigimgurl='{original}' hdlength='1000' cdnmidimgurl='{standard}' length='500' aeskey='{aes}' md5='{digest}'/></msg>"
    );
    let resource = image_resource(&xml).unwrap();
    assert_eq!(
        (resource.kind, resource.file_id.as_str(), resource.length),
        ("image", original.as_str(), 1000)
    );
    let resource = image_resource(&xml.replace("hdlength='1000'", "hdlength='0'")).unwrap();
    assert_eq!(
        (resource.kind, resource.file_id.as_str(), resource.length),
        ("image_mid", standard.as_str(), 500)
    );
    for attrs in [
        format!("cdnbigimgurl='{original}' length='500'"),
        format!("cdnmidimgurl='{standard}' hdlength='1000'"),
        format!("cdnthumburl='{standard}' cdnthumblength='500' cdnthumbaeskey='{aes}'"),
        format!("cdnmidimgurl='{standard}' length='0'"),
        format!("cdnmidimgurl='invalid' length='500'"),
    ] {
        assert!(
            image_resource(&format!(
                "<msg><img {attrs} aeskey='{aes}' md5='{digest}'/></msg>"
            ))
            .is_err()
        );
    }
    for (from, to) in [(aes.as_str(), "invalid"), (digest.as_str(), "invalid")] {
        assert!(image_resource(&xml.replace(from, to)).is_err());
    }
}

#[test]
fn transcoded_video_cache_requires_the_same_download_and_a_matching_cdn_checksum() {
    let root = tempfile::tempdir().unwrap();
    let group = "123456@chatroom";
    let bytes = b"verified CDN transcoded video bytes";
    let resource = DownloadResource {
        kind: "video",
        file_id: "a".repeat(194),
        aes: "b".repeat(32),
        length: 520420,
        md5: "c".repeat(32),
    };
    let path = root.path().join("video.mp4");
    let receipt = root.path().join("video-receipt.json");
    fs::write(&path, bytes).unwrap();
    atomic_write(&receipt,&serde_json::to_vec(&json!({"request_id":download_id(group,"video-key",&resource),"size":bytes.len(),"md5":picture::md5(bytes)})).unwrap()).unwrap();
    assert!(cached_video_receipt(
        &path,
        &receipt,
        group,
        "video-key",
        std::slice::from_ref(&resource)
    ));
    assert!(!cached_video_receipt(
        &path,
        &receipt,
        group,
        "different-message",
        std::slice::from_ref(&resource)
    ));
    fs::write(&path, b"corrupt").unwrap();
    assert!(!cached_video_receipt(
        &path,
        &receipt,
        group,
        "video-key",
        &[resource]
    ));
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
fn the_placeholder_picture_is_a_valid_decodable_image() {
    // The placeholder is what a user sees when a picture cannot be produced. It used to be
    // a truncated PNG: the client decoded it as nothing and showed "图片 · 点击下载", so a
    // failed decode looked like a picture that had never arrived. Assert the bytes decode,
    // report the right size, and survive the same validation real pictures go through.
    let (format, width, height) = picture::validate(PLACEHOLDER_PNG, 12000).unwrap();
    assert_eq!(format, image::ImageFormat::Png);
    assert_eq!((width, height), (320, 240));
    let root = tempfile::tempdir().unwrap();
    let path = Media::placeholder_picture(root.path()).unwrap();
    assert_eq!(fs::read(&path).unwrap(), PLACEHOLDER_PNG);
    assert!(picture::read_and_validate(&path, 12000).is_ok());
    // It must not be cached as `image.*`: that name is what a later successful decode
    // consults first, so a placeholder there would keep hiding the real picture.
    assert!(cached_picture(root.path()).is_none());
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
    let result = media.inbound(&mut db, group, &row, "image").unwrap();
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
    let result = media.inbound(&mut db, group, &row, "file").unwrap();
    assert_eq!(result["name"], "original.bin");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), file);
    let video = b"\0\0\0\x20ftypisomoffline_video_fixture";
    let video_root = account.join("msg/video/2026-10");
    fs::create_dir_all(&video_root).unwrap();
    fs::write(video_root.join("cached.mp4"), video).unwrap();
    let row = json!({"local_id":3,"sort_seq":3,"shard":"offline","content":format!("<msg><videomsg length='{}' md5='{}'/></msg>",video.len(),picture::md5(video))});
    let result = media.inbound(&mut db, group, &row, "video").unwrap();
    assert_eq!(result["kind"], "video");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), video);
    let gif = gif(16);
    let digest = picture::md5(&gif);
    let emoji_root = account.join("emoticon");
    fs::create_dir_all(&emoji_root).unwrap();
    fs::write(emoji_root.join(format!("{digest}.gif")), &gif).unwrap();
    let row = json!({"local_id":4,"sort_seq":4,"shard":"offline","content":format!("<msg><emoji md5='{digest}' len='{}'/></msg>",gif.len())});
    let result = media.inbound(&mut db, group, &row, "gif").unwrap();
    assert_eq!(result["kind"], "gif");
    assert_eq!(fs::read(result["path"].as_str().unwrap()).unwrap(), gif);
}

#[test]
fn completed_video_partial_and_destination_skip_archive_but_still_verify_content() {
    let root = tempfile::tempdir().unwrap();
    let account = root.path().join("account");
    let video_root = account.join("msg/video/archive");
    fs::create_dir_all(&video_root).unwrap();
    let video = b"\0\0\0\x20ftypisomoffline_video_fixture";
    let corrupt = vec![b'x'; video.len()];
    // A fallback archive exists, so zero visited entries proves the owned-file fast path.
    for index in 0..32 {
        fs::write(video_root.join(format!("other-{index}.mp4")), &corrupt).unwrap();
    }
    fs::write(video_root.join("cached.mp4"), video).unwrap();
    let db = Db::media_fixture(account, "wxid_offline", None, Default::default());
    let mut media = Media::new(root.path().join("adapter"));
    let group = "123456@chatroom";
    let row = json!({"local_id":3,"sort_seq":3,"shard":"offline","content":format!("<msg><videomsg length='{}' md5='{}'/></msg>",video.len(),picture::md5(video))});
    let key = message_key(group, &row);
    let request_key = format!("video:{key}");
    let destination = media.cache_dir(&request_key).unwrap().join("video.mp4");
    let partial = media
        .directory
        .join("feishu/media/wechat-video")
        .join(format!("{}.mp4.part", sha256(request_key.as_bytes())));
    fs::create_dir_all(partial.parent().unwrap()).unwrap();
    fs::write(&partial, video).unwrap();
    fs::write(&destination, &corrupt).unwrap();
    let restored = media.video(&db, group, &row, &key).unwrap();
    assert_eq!(restored, destination);
    assert_eq!(fs::read(&restored).unwrap(), video);
    assert_eq!(media.video_scan_entries.get(), 0);

    fs::remove_file(&partial).unwrap();
    assert_eq!(media.video(&db, group, &row, &key).unwrap(), destination);
    assert_eq!(media.video_scan_entries.get(), 0);
    // A same-size corrupted private file must not become a successful fast-path hit.
    fs::write(&destination, &corrupt).unwrap();
    assert_eq!(media.video(&db, group, &row, &key).unwrap(), destination);
    assert!(media.video_scan_entries.get() > 0);
    assert_eq!(fs::read(destination).unwrap(), video);
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
        let error = media.inbound(&mut db, group, &row, kind).unwrap_err();
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
