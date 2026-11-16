use super::*;
use md5::{Digest, Md5};
use rusqlite::{Connection, params};
use tempfile::TempDir;

#[test]
fn announcement_author_and_local_avatar_are_resolved_without_enabling_native_sending() {
    let temp = TempDir::new().unwrap();
    let contact = Connection::open_in_memory().unwrap();
    contact.execute_batch("CREATE TABLE contact(username TEXT,remark TEXT,nick_name TEXT);CREATE TABLE chat_room(username TEXT,ext_buffer BLOB);CREATE TABLE chat_room_info_detail(username_ TEXT,announcement_,announcement_editor_,announcement_publish_time_);INSERT INTO contact VALUES('fixture-editor','Fixture remark','Fixture nick');INSERT INTO chat_room_info_detail VALUES('123456@chatroom','Fixture announcement','fixture-editor',1760000000);").unwrap();
    let heads = Connection::open_in_memory().unwrap();
    heads
        .execute_batch("CREATE TABLE head_image(username TEXT,image_buffer BLOB)")
        .unwrap();
    let mut image = std::io::Cursor::new(Vec::new());
    image::DynamicImage::new_rgb8(2, 2)
        .write_to(&mut image, image::ImageFormat::Png)
        .unwrap();
    heads
        .execute(
            "INSERT INTO head_image VALUES('fixture-editor',?1)",
            [image.into_inner()],
        )
        .unwrap();
    let mut access = Local {
        db: Db::media_fixture(
            temp.path().join("fixture-account"),
            "fixture-self",
            None,
            HashMap::from([
                ("contact/contact.db".into(), contact),
                ("head_image/head_image.db".into(), heads),
            ]),
        ),
        native: NativeClient::new(temp.path().join("native")),
        media: Media::new(temp.path().to_owned()),
        allowed: BTreeSet::new(),
        directory: temp.path().to_owned(),
        avatars: HashMap::new(),
        avatar_order: VecDeque::new(),
        actions: None,
        actions_error: String::new(),
        quotes: None,
        quotes_error: String::new(),
        quote_depth: 0,
            quote_failure: String::new(),
    };
    let announcement = access.announcement("123456@chatroom").unwrap();
    assert_eq!(announcement["available"], true);
    assert_eq!(announcement["author"], "Fixture remark");
    assert_eq!(announcement["author_id"], "fixture-editor");
    use base64::Engine as _;
    let avatar = base64::engine::general_purpose::STANDARD
        .decode(text(&announcement, "avatar"))
        .unwrap();
    assert!(avatar.starts_with(&[0xff, 0xd8, 0xff]));
    let picture = image::load_from_memory(&avatar).unwrap();
    assert_eq!((picture.width(), picture.height()), (64, 64));
    assert_eq!(access.avatars.len(), 1);
    assert!(access.allowed.is_empty());
    assert!(access.native.pid.is_none());
    assert!(access.actions.is_none());
    assert!(access.quotes.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn timestamps() {
    assert_eq!(timestamp(&json!(1_760_000_000)).unwrap(), 1_760_000_000_000);
    assert_eq!(
        timestamp(&json!(1_760_000_000_123_i64)).unwrap(),
        1_760_000_000_123
    );
    assert_eq!(
        timestamp(&json!("1970-01-02T00:00:00Z")).unwrap(),
        86_400_000
    );
    assert!(timestamp(&json!("invalid")).is_err());
}

#[test]
fn sender_envelope_uses_only_the_first_line_and_cannot_override_authority() {
    let group = "123456@chatroom";
    let resolver =
        |_: &str| -> Result<String> { panic!("an authoritative actor needs no name lookup") };
    let row = json!({"sender_id":2,"content":"wxid_self:\r\nline ending colon:\nlater"});
    assert_eq!(
        message_identity(group, &row, "wxid_self", resolver).unwrap(),
        ("line ending colon:\nlater".into(), "wxid_self".into())
    );
    for body in ["wxid_other:\nspoofed", "first line\nwxid_self:\nbody"] {
        let row = json!({"sender_id":2,"content":body});
        assert_eq!(
            message_identity(group, &row, "wxid_self", resolver).unwrap(),
            (body.into(), "wxid_self".into())
        );
    }
    let row = json!({"sender_id":7,"sender_username":"member-a","content":"member-b:\nspoofed"});
    assert_eq!(
        message_identity(group, &row, "wxid_self", resolver).unwrap(),
        ("member-b:\nspoofed".into(), "member-a".into())
    );
    let row = json!({"sender_id":7,"content":"known-member:\n正文"});
    assert_eq!(
        message_identity(group, &row, "wxid_self", |id| Ok(if id == "known-member" {
            "成员"
        } else {
            id
        }
        .into()))
        .unwrap(),
        ("正文".into(), "known-member".into())
    );
}

#[test]
fn blob_source_keeps_incoming_mentions_and_confirms_outgoing_mentions() {
    assert_blob_source(b"<msgsource><atuserlist><![CDATA[wxid_self,wxid_self,notify@all]]></atuserlist></msgsource>");
}

#[test]
fn zstd_blob_source_keeps_incoming_mentions_and_confirms_outgoing_mentions() {
    let source = zstd::stream::encode_all(
        b"<msgsource><atuserlist><![CDATA[wxid_self,wxid_self,notify@all]]></atuserlist></msgsource>".as_slice(),
        1,
    ).unwrap();
    assert_blob_source(&source);
}

fn assert_blob_source(source: &[u8]) {
    const GROUP: &str = "123456@chatroom";
    let temp = TempDir::new().unwrap();
    let contact = Connection::open_in_memory().unwrap();
    contact.execute_batch("CREATE TABLE contact(username TEXT,remark TEXT,nick_name TEXT);CREATE TABLE chat_room(username TEXT,ext_buffer BLOB);INSERT INTO contact VALUES('wxid_member','','成员'),('wxid_self','','Nick');").unwrap();
    let messages = Connection::open_in_memory().unwrap();
    let table = format!("Msg_{:x}", Md5::digest(GROUP.as_bytes()));
    messages.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);INSERT INTO Name2Id(rowid,user_name) VALUES(7,'wxid_member');CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content TEXT,source BLOB,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);")).unwrap();
    for (id, sender) in [(1, 7), (2, 2)] {
        messages
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,1,?2,1000,?3,?4,NULL,NULL,42,?1)"),
                params![id, sender, "@Nick\u{2005}你好", source],
            )
            .unwrap();
    }
    let db = Db::media_fixture(
        temp.path().join("fake-account"),
        "wxid_self",
        None,
        HashMap::from([
            ("contact/contact.db".into(), contact),
            ("message/message_0.db".into(), messages),
        ]),
    );
    let mut access = Local {
        db,
        native: NativeClient::new(temp.path().join("native")),
        media: Media::new(temp.path().to_owned()),
        allowed: BTreeSet::new(),
        directory: temp.path().to_owned(),
        avatars: HashMap::new(),
        avatar_order: VecDeque::new(),
        actions: None,
        actions_error: String::new(),
        quotes: None,
        quotes_error: String::new(),
        quote_depth: 0,
            quote_failure: String::new(),
    };
    let rows = access
        .db
        .page(GROUP, &json!({"sort_seq":0,"shard":"","local_id":0}), 2)
        .unwrap();
    assert_eq!(access.self_id(), "wxid_self");
    assert!(rows[0]["source"].is_array());
    let converted = access.convert(GROUP, &rows[0]).unwrap().unwrap();
    assert_eq!(
        converted["mentions"],
        json!([{"id":"wxid_self","name":"Nick"},{"id":"notify@all","name":"所有人"}])
    );
    let mut expected = json!({"kind":"text","text":"@Nick\u{2005}你好","atlist":"wxid_self"});
    assert!(access.matches(GROUP, &rows[1], &expected).unwrap());
    let mut fallback = rows[1].clone();
    fallback.as_object_mut().unwrap().remove("source");
    assert!(access.matches(GROUP, &fallback, &expected).unwrap());
    assert!(!access.matches(GROUP, &rows[0], &expected).unwrap());
    expected["text"] = json!("different body");
    assert!(!access.matches(GROUP, &rows[1], &expected).unwrap());
    expected["text"] = rows[1]["content"].clone();
    expected["atlist"] = json!("wrong-member");
    assert!(!access.matches(GROUP, &rows[1], &expected).unwrap());
    assert!(source_atlist(&json!({"source":[255,0]})).is_empty());
    assert!(source_atlist(&json!({"source":[256,-1]})).is_empty());
}

#[test]
fn compressed_source_rejects_corrupt_oversized_and_non_utf8_data_without_fallback() {
    let valid = zstd::stream::encode_all(
        b"<msgsource><atuserlist>wxid_self</atuserlist></msgsource>".as_slice(),
        1,
    )
    .unwrap();
    for length in [4, valid.len() / 2, valid.len() - 1] {
        assert!(source_text(&json!(valid[..length])).is_empty());
    }
    let oversized =
        zstd::stream::encode_all(vec![b'x'; 2 * 1024 * 1024 + 1].as_slice(), 1).unwrap();
    assert!(source_text(&json!(oversized)).is_empty());
    let invalid_utf8 = zstd::stream::encode_all([255u8, 0].as_slice(), 1).unwrap();
    assert!(source_text(&json!(invalid_utf8)).is_empty());
    assert!(source_text(&json!("x".repeat(2 * 1024 * 1024 + 1))).is_empty());
}

#[test]
fn health_failure_before_submission_is_a_definite_rejection() {
    for value in [
        Ok(json!({"pid":8})),
        Ok(json!({"pid":7,"error":"account changed"})),
        Ok(json!({})),
        Err(anyhow::anyhow!("GET health disconnected")),
    ] {
        let error = submission_preflight(true, Some(7), || value).unwrap_err();
        assert!(error.downcast_ref::<Rejected>().is_some());
        assert!(error.to_string().contains("未提交"));
    }
    let error = submission_preflight(false, Some(7), || {
        panic!("unauthorized preflight must not even query health")
    })
    .unwrap_err();
    assert!(error.downcast_ref::<Rejected>().is_some());
    assert!(submission_preflight(true, None, || Ok(json!({}))).is_err());
    for error in [Value::Null, json!(false), json!(0), json!("")] {
        submission_preflight(true, Some(7), || Ok(json!({"pid":7,"error":error}))).unwrap();
    }
}

const RECALL_GROUP: &str = "123456@chatroom";

fn recall_db_fixture() -> (TempDir, Local, String) {
    let temp = TempDir::new().unwrap();
    let contacts = Connection::open_in_memory().unwrap();
    contacts.execute_batch("CREATE TABLE contact(username TEXT,remark TEXT,nick_name TEXT);CREATE TABLE chat_room(username TEXT,ext_buffer BLOB);").unwrap();
    let messages = Connection::open_in_memory().unwrap();
    let table = format!("Msg_{:x}", Md5::digest(RECALL_GROUP.as_bytes()));
    messages.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content TEXT,source BLOB,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);INSERT INTO {table} VALUES(3,1,2,1000,'fixture message',NULL,NULL,NULL,-1,30);")).unwrap();
    let access = Local {
        db: Db::media_fixture(
            temp.path().join("fixture-account"),
            "wxid_self",
            None,
            HashMap::from([
                ("message/message_0.db".into(), messages),
                ("contact/contact.db".into(), contacts),
            ]),
        ),
        native: NativeClient::new(temp.path().join("native")),
        media: Media::new(temp.path().to_owned()),
        allowed: BTreeSet::from([RECALL_GROUP.into()]),
        directory: temp.path().to_owned(),
        avatars: HashMap::new(),
        avatar_order: VecDeque::new(),
        actions: None,
        actions_error: String::new(),
        quotes: None,
        quotes_error: String::new(),
        quote_depth: 0,
            quote_failure: String::new(),
    };
    (temp, access, table)
}

/// Rows for one group, so a test can describe a whole conversation compactly.
/// Each entry is (local_id, local_type, server_id, body).
fn group_with_rows(group: &str, rows: &[(i64, i64, i64, &str)]) -> (TempDir, Local, String) {
    let temp = TempDir::new().unwrap();
    let contacts = Connection::open_in_memory().unwrap();
    contacts
        .execute_batch(
            "CREATE TABLE contact(username TEXT,remark TEXT,nick_name TEXT);
             CREATE TABLE chat_room(username TEXT,ext_buffer BLOB);",
        )
        .unwrap();
    let messages = Connection::open_in_memory().unwrap();
    let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
    messages
        .execute_batch(&format!(
            "CREATE TABLE Name2Id(user_name TEXT);
             CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,
                real_sender_id INTEGER,create_time INTEGER,message_content TEXT,
                source BLOB,packed_info_data BLOB,compress_content BLOB,
                server_id INTEGER,sort_seq INTEGER);"
        ))
        .unwrap();
    for (local_id, local_type, server_id, body) in rows {
        messages
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,?2,2,1000,?3,NULL,NULL,NULL,?4,?5)"),
                rusqlite::params![local_id, local_type, body, server_id, local_id * 10],
            )
            .unwrap();
    }
    let access = Local {
        db: Db::media_fixture(
            temp.path().join("fixture-account"),
            "wxid_self",
            None,
            HashMap::from([
                ("message/message_0.db".into(), messages),
                ("contact/contact.db".into(), contacts),
            ]),
        ),
        native: NativeClient::new(temp.path().join("native")),
        media: Media::new(temp.path().to_owned()),
        allowed: BTreeSet::from([group.to_owned()]),
        directory: temp.path().to_owned(),
        avatars: HashMap::new(),
        avatar_order: VecDeque::new(),
        actions: None,
        actions_error: String::new(),
        quotes: None,
        quotes_error: String::new(),
        quote_depth: 0,
        quote_failure: String::new(),
    };
    (temp, access, table)
}

#[test]
fn received_thumbnail_sets_message_pending_until_the_full_picture_is_available() {
    let digest = "d".repeat(32);
    let content = format!("<msg><img md5='{digest}'/></msg>");
    let (temp, mut access, _) = group_with_rows(RECALL_GROUP, &[(3, 3, 42, &content)]);
    let mut png = std::io::Cursor::new(Vec::new());
    image::DynamicImage::new_rgb8(2, 2)
        .write_to(&mut png, image::ImageFormat::Png)
        .unwrap();
    let png = png.into_inner();
    let folder = access
        .db
        .account_dir
        .join("msg/attach")
        .join(format!("{:x}", Md5::digest(RECALL_GROUP.as_bytes())))
        .join("2026-11/Img");
    std::fs::create_dir_all(&folder).unwrap();
    std::fs::write(folder.join(format!("{digest}_t.dat")), &png).unwrap();
    let row = access
        .db
        .reload(
            RECALL_GROUP,
            &json!({"local_id":3,"sort_seq":30,"shard":"message__message_0.db"}),
        )
        .unwrap();
    let preview = access.convert(RECALL_GROUP, &row).unwrap().unwrap();
    assert_eq!(preview["media_pending"], true);
    assert_eq!(preview["media"][0]["name"], "preview.png");
    std::fs::write(folder.join(format!("{digest}.dat")), png).unwrap();
    let full = access.convert(RECALL_GROUP, &row).unwrap().unwrap();
    assert_ne!(full["media_pending"], true);
    assert_eq!(full["media"][0]["name"], "image.png");
    assert_eq!(full["id"], preview["id"]);
    assert_eq!(full["source_id"], preview["source_id"]);
    assert!(access.native.pid.is_none() && !temp.path().join("native").exists());
}

#[test]
fn received_image_quote_uses_a_label_and_real_source_even_before_media_is_cached() {
    let (temp, mut access, table) = recall_db_fixture();
    let mut png = std::io::Cursor::new(Vec::new());
    image::DynamicImage::new_rgb8(2, 2)
        .write_to(&mut png, image::ImageFormat::Png)
        .unwrap();
    let png = png.into_inner();
    let digest = format!("{:x}", Md5::digest(&png));
    let image = format!("<?xml version=\"1.0\"?><msg><img md5=\"{digest}\"/></msg>");
    let reference_image =
        format!("<msg><img cdnmidimgurl='refreshed-download' md5='{digest}'/></msg>");
    let card = format!(
        "<msg><appmsg><type>57</type><title>这个</title><refermsg><type>3</type><svrid>18446744073709551615</svrid><fromusr>wxid_self</fromusr><chatusr>wxid_self</chatusr><displayname>成员</displayname><content><![CDATA[{reference_image}]]></content></refermsg></appmsg></msg>"
    );
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("UPDATE {table} SET local_type=3,message_content=?1 WHERE local_id=3"),
            [&image],
        )
        .unwrap();
    let reply = json!({"local_id":4,"sort_seq":40,"shard":"message__message_0.db","sender_id":2,"create_time":1001,"type":"文件/链接/卡片","content":card});
    let converted = access.convert(RECALL_GROUP, &reply).unwrap().unwrap();
    assert_eq!(converted["text"], "这个");
    assert_eq!(converted["quote"]["text"], "[图片]");
    let expected_source = format!("wx:{RECALL_GROUP}:message__message_0.db:3:30");
    assert_eq!(converted["_quote_source"], expected_source);
    assert_eq!(converted["_quote_server_id"], "18446744073709551615");
    assert_eq!(
        converted["quote"]["id"],
        Uuid::new_v5(&Uuid::NAMESPACE_URL, expected_source.as_bytes()).to_string()
    );
    assert!(converted.get("_quote_original").is_none()); // An uncached image must not hold up its reply.
    let row = access
        .db
        .reload(
            RECALL_GROUP,
            &json!({"local_id":3,"sort_seq":30,"shard":"message__message_0.db"}),
        )
        .unwrap();
    let raw = access
        .db
        .raw_message(RECALL_GROUP, 3, "message__message_0.db")
        .unwrap();
    assert!(crate::quotes::prepare_source(RECALL_GROUP, &row, &raw, "wxid_self").is_err());
    let folder = temp
        .path()
        .join("fixture-account/msg/attach")
        .join(format!("{:x}", Md5::digest(RECALL_GROUP.as_bytes())))
        .join("2026-10/Img");
    std::fs::create_dir_all(&folder).unwrap();
    std::fs::write(
        folder.join(format!("{digest}.dat")),
        png.iter().map(|b| b ^ 0x88).collect::<Vec<_>>(),
    )
    .unwrap();
    let ready = access.convert(RECALL_GROUP, &reply).unwrap().unwrap();
    assert_eq!(ready["quote"], converted["quote"]);
    assert_eq!(ready["_quote_original"]["id"], ready["quote"]["id"]);
    assert_eq!(ready["_quote_original"]["media"][0]["kind"], "image");
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn received_link_quote_resolves_the_real_row_and_stores_a_readable_preview() {
    let (temp, mut access, table) = recall_db_fixture();
    let link = "<msg><appmsg appid=\"\" sdkver=\"0\"><title>一篇文章</title><type>5</type><url>https://example.invalid/private?key=secret</url><des>摘要</des></appmsg></msg>";
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(5,49,2,1002,?1,NULL,NULL,NULL,777,35)"),
            [link],
        )
        .unwrap();

    let card = format!(
        "<msg><appmsg><type>57</type><title>这个不错</title><refermsg><type>5</type><svrid>777</svrid><fromusr>wxid_self</fromusr><chatusr>wxid_self</chatusr><displayname>鸳鸯茶</displayname><content><![CDATA[{link}]]></content></refermsg></appmsg></msg>"
    );
    let reply = json!({"local_id":6,"sort_seq":40,"shard":"message__message_0.db","sender_id":2,"create_time":1003,"type":"文件/链接/卡片","content":card});
    let converted = access.convert(RECALL_GROUP, &reply).unwrap().unwrap();
    assert_eq!(converted["text"], "这个不错");
    assert_eq!(converted["quote"]["sender"], "鸳鸯茶");
    assert_eq!(converted["quote"]["text"], "[链接] 一篇文章");
    let expected_source = format!("wx:{RECALL_GROUP}:message__message_0.db:5:35");
    assert_eq!(converted["quote"]["id"], Uuid::new_v5(&Uuid::NAMESPACE_URL, expected_source.as_bytes()).to_string());
    assert_eq!(converted["_quote_source"], expected_source);
    // The linked article itself still renders as its own message (title, summary, URL);
    // only the reference preview is reduced to a label.
    assert_eq!(converted["_quote_original"]["text"], "一篇文章\n摘要\nhttps://example.invalid/private?key=secret");
    let serialized = converted.to_string();
    assert!(!serialized.contains("appid") && !serialized.contains("sdkver"));
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn quote_resolves_by_server_id_even_when_wechat_rewrote_the_preview() {
    let (temp, mut access, table) = recall_db_fixture();
    // The stored body and the card's quoted preview disagree on whitespace, the sender
    // envelope and the display name. That used to reject the reference outright; the
    // lookup must now go by the message number alone.
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("UPDATE {table} SET message_content='  热热热  \\r\\n' WHERE local_id=3"),
            [],
        )
        .unwrap();
    let reference = json!({
        "type": 1, "server_id": "18446744073709551615",
        "fromusr": RECALL_GROUP, "chatusr": "someone-else",
        "sender": "改过的昵称", "text": "热热热"
    });
    let original = access
        .referenced_original(RECALL_GROUP, &reference)
        .unwrap()
        .expect("a unique server_id must resolve without comparing the body");
    assert_eq!(original["local_id"], 3);
    assert!(access.quote_failure.is_empty());

    // A number that is not present says so instead of a generic failure.
    let missing = json!({"type": 1, "server_id": "424242", "fromusr": RECALL_GROUP, "chatusr": "x", "text": "y"});
    assert!(access.referenced_original(RECALL_GROUP, &missing).unwrap().is_none());
    assert!(access.quote_failure.contains("no stored row"));

    // A recalled original stays unresolvable, and the reason is specific.
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(&format!("UPDATE {table} SET local_type=10000,message_content='' WHERE local_id=3"), [])
        .unwrap();
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn media_quotes_resolve_by_server_id_without_any_content_comparison() {
    // Images, videos and files are quoted constantly, and their preview text ("[图片]",
    // "[视频]", a bare filename) repeats across a conversation, so content can never
    // identify them. The WeChat message number is the only usable key.
    let (temp, mut access, table) = recall_db_fixture();
    let cases: [(i64, &str, &str, &str); 3] = [
        (3, "图片", "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>", "[图片]"),
        (43, "视频", "<msg><videomsg length='12000' playlength='12'/></msg>", "[视频]"),
        (49, "文件/链接/卡片", "<msg><appmsg><title>季度报告.pdf</title><type>6</type></appmsg></msg>", "[文件] 季度报告.pdf"),
    ];
    for (index, (local_type, kind, body, preview)) in cases.into_iter().enumerate() {
        let server = 900 + index as i64;
        access
            .db
            .open("message/message_0.db")
            .unwrap()
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,?2,2,1000,?3,NULL,NULL,NULL,?4,40)"),
                rusqlite::params![10 + index as i64, local_type, body, server],
            )
            .unwrap();
        // The card's own preview text deliberately does not resemble the stored body.
        let reference = json!({
            "type": local_type, "server_id": server.to_string(),
            "fromusr": RECALL_GROUP, "chatusr": "wxid_other", "text": preview
        });
        let original = access
            .referenced_original(RECALL_GROUP, &reference)
            .unwrap()
            .unwrap_or_else(|| panic!("{kind} quote must resolve by server_id"));
        assert_eq!(original["type"], kind);
    }
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn every_quote_kind_resolves_to_the_exact_row_named_by_its_message_number() {
    // The reason this matters: preview text repeats ("[图片]" dozens of times, "收到" hundreds)
    // but the WeChat message number is unique, so it is the only key that cannot pick the
    // wrong row. Every type a group actually quotes is covered here.
    let body_image = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
    let body_video = "<msg><videomsg length='12000' playlength='12'/></msg>";
    let body_voice = "<msg><voicemsg length='3000' voiceformat='4'/></msg>";
    let body_sticker = "<msg><emoji md5='0123456789abcdef0123456789abcdef'/></msg>";
    let body_file = "<msg><appmsg><title>季度报告.pdf</title><type>6</type><totallen>2048</totallen></appmsg></msg>";
    let body_link = "<msg><appmsg><title>一篇文章</title><type>5</type><url>https://example.invalid/a</url></appmsg></msg>";
    let body_record = "<msg><appmsg><title>群聊的聊天记录</title><type>19</type></appmsg></msg>";
    let body_location = "<msg><location x='1.0' y='2.0' label='某地'/></msg>";
    let (temp, mut access, _) = group_with_rows(
        RECALL_GROUP,
        &[
            (3, 1, 501, "第一条"),
            (4, 3, 502, body_image),
            (5, 43, 503, body_video),
            (6, 34, 504, body_voice),
            (7, 47, 505, body_sticker),
            (8, 49, 506, body_file),
            (9, 49, 507, body_link),
            (10, 49, 508, body_record),
            (11, 48, 509, body_location),
            (12, 1, 510, "第一条"), // the same text as row 3: content alone would be ambiguous
        ],
    );
    // (quote type, expected local_id, preview the card carries)
    let cases: [(u64, i64, &str); 10] = [
        (1, 3, "第一条"),
        (1, 12, "第一条"),
        (3, 4, "[图片]"),
        (43, 5, "[视频]"),
        (34, 6, "[语音]"),
        (47, 7, "[动画表情]"),
        (49, 8, "[文件] 季度报告.pdf"),
        (49, 9, "[链接] 一篇文章"),
        (49, 10, "[聊天记录] 群聊的聊天记录"),
        (48, 11, "[位置]"),
    ];
    for (kind, expected_local_id, preview) in cases {
        let server = 501 + (expected_local_id - 3) as i64;
        let reference = json!({
            "type": kind, "server_id": server.to_string(),
            // Deliberately wrong actor and a preview that does not equal the stored body.
            "fromusr": RECALL_GROUP, "chatusr": "wxid_someone_else",
            "sender": "改过的昵称", "text": preview
        });
        let row = access
            .referenced_original(RECALL_GROUP, &reference)
            .unwrap()
            .unwrap_or_else(|| panic!("type {kind} at server {server} must resolve"));
        assert_eq!(
            row["local_id"].as_i64(),
            Some(expected_local_id),
            "type {kind} resolved to the wrong row"
        );
        assert!(access.quote_failure.is_empty(), "unexpected failure: {}", access.quote_failure);
    }
    // The two identical texts resolved to different rows because the number decided.
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn repeated_previews_never_resolve_to_the_wrong_row() {
    // Ten images in a row, all previewed as "[图片]". Each quote must land on its own row.
    let body = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
    let rows: Vec<(i64, i64, i64, &str)> =
        (0..10).map(|i| (20 + i, 3, 700 + i, body)).collect();
    let (temp, mut access, _) = group_with_rows(RECALL_GROUP, &rows);
    for i in 0..10 {
        let reference = json!({
            "type": 3, "server_id": (700 + i).to_string(),
            "fromusr": RECALL_GROUP, "chatusr": "wxid_someone", "text": "[图片]"
        });
        let row = access.referenced_original(RECALL_GROUP, &reference).unwrap().unwrap();
        assert_eq!(row["local_id"].as_i64(), Some(20 + i), "image {i} resolved wrongly");
    }
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn a_quote_never_resolves_into_a_different_group() {
    // Same message number in two groups. The lookup for one group must not see the other.
    let (temp, mut access, _) =
        group_with_rows(RECALL_GROUP, &[(3, 1, 900, "这个群的原文")]);
    let other = "98765432101@chatroom";
    let other_body = "另一个群的原文";
    // Build the second group's table inside the same database file used for the first.
    let table = format!("Msg_{:x}", Md5::digest(other.as_bytes()));
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute_batch(&format!(
            "CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,
                real_sender_id INTEGER,create_time INTEGER,message_content TEXT,
                source BLOB,packed_info_data BLOB,compress_content BLOB,
                server_id INTEGER,sort_seq INTEGER);
             INSERT INTO {table} VALUES(1,1,2,1000,'{other_body}',NULL,NULL,NULL,900,10);"
        ))
        .unwrap();
    let reference = json!({
        "type": 1, "server_id": "900",
        "fromusr": other, "chatusr": "wxid_someone", "text": other_body
    });
    let row = access
        .referenced_original(RECALL_GROUP, &reference)
        .unwrap()
        .expect("the row belongs to this group");
    assert_eq!(row["local_id"].as_i64(), Some(3), "must read this group's row, not the other's");
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn unresolved_quotes_report_the_exact_stage() {
    let (temp, mut access, _) = group_with_rows(RECALL_GROUP, &[(3, 1, 900, "存在")]);
    // Unknown number.
    let missing = json!({"type": 1, "server_id": "123456", "fromusr": RECALL_GROUP,
        "chatusr": "x", "text": "y"});
    assert!(access.referenced_original(RECALL_GROUP, &missing).unwrap().is_none());
    assert!(access.quote_failure.contains("no stored row"), "{}", access.quote_failure);
    // Zero and non-numeric numbers are rejected before any query.
    for bad in ["0", "", "not-a-number"] {
        let reference = json!({"type": 1, "server_id": bad, "fromusr": RECALL_GROUP,
            "chatusr": "x", "text": "y"});
        assert!(access.referenced_original(RECALL_GROUP, &reference).unwrap().is_none());
        assert!(access.quote_failure.contains("no usable server_id"), "{}", access.quote_failure);
    }
    // A successful lookup clears the previous reason, so a stale message cannot linger.
    let ok = json!({"type": 1, "server_id": "900", "fromusr": RECALL_GROUP,
        "chatusr": "x", "text": "存在"});
    assert!(access.referenced_original(RECALL_GROUP, &ok).unwrap().is_some());
    assert!(access.quote_failure.is_empty());
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn a_duplicate_message_number_is_refused_rather_than_guessed() {
    let (temp, mut access, table) = group_with_rows(RECALL_GROUP, &[(3, 1, 900, "第一行")]);
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(4,1,2,1000,'第二行',NULL,NULL,NULL,900,40)"),
            [],
        )
        .unwrap();
    let reference = json!({"type": 1, "server_id": "900", "fromusr": RECALL_GROUP,
        "chatusr": "x", "text": "y"});
    assert!(access.referenced_original(RECALL_GROUP, &reference).unwrap().is_none());
    assert!(access.quote_failure.contains("not unique"), "{}", access.quote_failure);
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn a_received_media_reply_reaches_the_client_with_a_real_source_id() {
    // End to end through convert(): the reply row is a real type-57 card, the original is a
    // picture, and the client must receive a usable quote id plus the proof fields. This is
    // the shape the desktop client relies on to locate the original.
    let image = "<msg><img md5='0123456789abcdef0123456789abcdef'/></msg>";
    let (temp, mut access, table) = group_with_rows(
        RECALL_GROUP,
        &[(3, 3, 4242, image), (4, 1, 4243, "先前的文字")],
    );
    // A reply whose quoted content does NOT match the stored body: WeChat rewrites previews.
    let card = "<msg><appmsg><type>57</type><title>这张图</title><refermsg><type>3</type><svrid>4242</svrid>\
<fromusr>wxid_other</fromusr><chatusr>wxid_other</chatusr><displayname>别人</displayname>\
<content><![CDATA[<msg><img md5='0123456789abcdef0123456789abcdef' cdnmidimgurl='resigned'/></msg>]]></content>\
</refermsg></appmsg></msg>";
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(5,49,2,2000,?1,NULL,NULL,NULL,4300,50)"),
            [card],
        )
        .unwrap();
    let reply = access
        .db
        .reload(
            RECALL_GROUP,
            &json!({"local_id":5,"sort_seq":50,"shard":"message__message_0.db"}),
        )
        .unwrap();
    let converted = access.convert(RECALL_GROUP, &reply).unwrap().unwrap();
    let expected_source = format!("wx:{RECALL_GROUP}:message__message_0.db:3:30");
    assert_eq!(converted["_quote_source"], expected_source);
    assert_eq!(
        converted["quote"]["id"],
        Uuid::new_v5(&Uuid::NAMESPACE_URL, expected_source.as_bytes()).to_string(),
        "the quote must carry the real source id, not an unavailable- placeholder"
    );
    assert_eq!(converted["quote"]["text"], "[图片]");
    assert_eq!(converted["_quote_server_id"], "4242");
    // A picture whose media has not been cached yet must not hold up the reply: the source
    // id is what the client needs, and _quote_original only appears once media is ready.
    assert!(
        converted.get("_quote_original").is_none(),
        "an uncached image must not block the reply"
    );
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn a_reply_to_an_unknown_number_stays_a_placeholder_and_says_why() {
    let (temp, mut access, table) = group_with_rows(RECALL_GROUP, &[(3, 1, 4242, "存在")]);
    let card = "<msg><appmsg><type>57</type><title>那个</title><refermsg><type>1</type><svrid>9999</svrid>\
<fromusr>wxid_other</fromusr><chatusr>wxid_other</chatusr><content>已经不在库里</content>\
</refermsg></appmsg></msg>";
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(5,49,2,2000,?1,NULL,NULL,NULL,4300,50)"),
            [card],
        )
        .unwrap();
    let reply = access
        .db
        .reload(
            RECALL_GROUP,
            &json!({"local_id":5,"sort_seq":50,"shard":"message__message_0.db"}),
        )
        .unwrap();
    let converted = access.convert(RECALL_GROUP, &reply).unwrap().unwrap();
    assert_eq!(converted["quote"]["id"], "unavailable-9999");
    assert_eq!(converted["quote"]["unavailable"], false);
    // The stage is available for the relay to report instead of a generic sentence.
    assert!(access.quote_failure.contains("no stored row"), "{}", access.quote_failure);
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn received_reference_rejects_duplicate_server_ids_instead_of_choosing_the_first_row() {
    let (temp, mut access, table) = recall_db_fixture();
    let reference = json!({"type":1,"server_id":"18446744073709551615","fromusr":"wxid_self","chatusr":"wxid_self","text":"fixture message"});
    assert!(
        access
            .referenced_original(RECALL_GROUP, &reference)
            .unwrap()
            .is_some()
    );
    access.db.open("message/message_0.db").unwrap().execute(
        &format!("INSERT INTO {table} SELECT 4,local_type,real_sender_id,create_time,message_content,source,packed_info_data,compress_content,server_id,40 FROM {table} WHERE local_id=3"), []).unwrap();
    assert!(
        access
            .referenced_original(RECALL_GROUP, &reference)
            .unwrap()
            .is_none()
    );
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn received_reference_rejects_the_same_server_id_in_different_shards() {
    let (temp, mut access, table) = recall_db_fixture();
    let mut shards = HashMap::new();
    for index in 0..2 {
        let db = Connection::open_in_memory().unwrap();
        db.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content TEXT,source BLOB,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);INSERT INTO {table} VALUES(3,1,2,1000,'fixture message',NULL,NULL,NULL,-1,30);")).unwrap();
        shards.insert(format!("message/message_{index}.db"), db);
    }
    access.db = Db::media_fixture(
        temp.path().join("duplicate-account"),
        "wxid_self",
        None,
        shards,
    );
    let reference = json!({"type":1,"server_id":"18446744073709551615","fromusr":"wxid_self","chatusr":"wxid_self","text":"fixture message"});
    assert!(
        access
            .referenced_original(RECALL_GROUP, &reference)
            .unwrap()
            .is_none()
    );
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn disconnected_read_probe_drop_leaves_neighbor_native_permissions_untouched() {
    let (temp, mut access, _) = recall_db_fixture();
    let neighbor = temp.path().join("neighbor-native/native.json");
    std::fs::create_dir_all(neighbor.parent().unwrap()).unwrap();
    let permissions =
        json!({"groups":[RECALL_GROUP],"token":"fixture-not-a-credential","port":1}).to_string();
    std::fs::write(&neighbor, &permissions).unwrap();
    let disconnected = temp.path().join("isolated-read-probe/never-connect-native");
    access.native = NativeClient::new(disconnected.clone());
    access.allowed.clear();
    assert!(access.native.pid.is_none());
    assert!(access.actions.is_none());
    assert!(access.quotes.is_none());
    drop(access); // Both Local::drop and NativeClient::drop restrict only the absent isolated root.
    assert!(!disconnected.exists());
    assert_eq!(std::fs::read_to_string(neighbor).unwrap(), permissions);
}

#[test]
fn received_quote_classification_uses_the_verified_sender_envelope_like_conversion() {
    let (temp, mut access, _) = recall_db_fixture();
    let card = "<msg><appmsg><type>57</type><title>reply</title><refermsg><type>3</type><svrid>42</svrid><fromusr>wxid_self</fromusr><chatusr>wxid_self</chatusr><content>[图片]</content></refermsg></appmsg></msg>";
    let mut reply = json!({"sender_id":2,"sender_username":"wxid_self","type":"文件/链接/卡片","content":format!("wxid_self:\r\n{card}")});
    assert!(access.is_quote(RECALL_GROUP, &reply));
    reply["content"] = json!(format!("wxid_other:\n{card}"));
    assert!(!access.is_quote(RECALL_GROUP, &reply));
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[cfg(windows)]
#[test]
#[ignore = "Explicit WECHAT_LITE_QUOTE_IDENTITIES and a private diagnosis parent ACL are required; only reads WeChat DB/local images, never connects native components"]
fn read_only_image_quote_identity_probe() {
    let Some(identity_path) = std::env::var_os("WECHAT_LITE_QUOTE_IDENTITIES") else {
        return; // Even an explicit --ignored invocation without the opt-in must have no side effects.
    };
    let result = (|| -> Result<Value> {
        let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../..")
            .canonicalize()?;
        let diagnosis = workspace.join(".state/diagnosis-v0.3.6").canonicalize()?;
        ensure!(
            diagnosis.starts_with(&workspace),
            "Probe diagnosis directory is outside the workspace"
        );
        let identities = PathBuf::from(identity_path).canonicalize()?;
        ensure!(
            identities.starts_with(&diagnosis),
            "Probe identities must remain private"
        );
        ensure!(
            std::fs::metadata(&identities)?.len() <= 1024 * 1024,
            "Probe identity list is too large"
        );
        let identities: Value = serde_json::from_slice(&std::fs::read(identities)?)?;
        let identities = identities
            .as_array()
            .context("Probe identity list must be an array")?;
        ensure!(
            !identities.is_empty() && identities.len() <= 200,
            "Probe identity count is invalid"
        );
        // The operator protects the diagnosis parent ACL before opting in; this fresh child inherits it.
        let directory = diagnosis.join(format!("readonly-quote-probe-{}", Uuid::new_v4()));
        std::fs::create_dir(&directory)?;
        ensure!(
            directory.canonicalize()?.starts_with(&diagnosis),
            "Probe directory escaped its parent"
        );
        let disconnected = directory.join("never-connect-native");
        ensure!(
            !disconnected.exists(),
            "Probe native directory must not exist"
        );
        let mut access = Local {
            db: Db::connect(&directory.join("wechat"))?,
            native: NativeClient::new(disconnected.clone()),
            media: Media::new(directory.clone()),
            allowed: BTreeSet::new(),
            directory,
            avatars: HashMap::new(),
            avatar_order: VecDeque::new(),
            actions: None,
            actions_error: String::new(),
            quotes: None,
            quotes_error: String::new(),
            quote_depth: 0,
            quote_failure: String::new(),
        };
        let (mut reloaded, mut parsed, mut matched, mut media_available) = (0, 0, 0, 0);
        let (mut classified, mut converted_source_matched) = (0, 0);
        let mut stages = std::collections::BTreeMap::<String, u64>::new();
        for identity in identities {
            let group = text(identity, "group");
            let Ok(position) = locator(group, text(identity, "source_id")) else {
                continue;
            };
            let Ok(reply) = access.db.reload(group, &position) else {
                continue;
            };
            reloaded += 1;
            if access.is_quote(group, &reply) {
                classified += 1;
            }
            let Ok((content, _)) = access.identity(group, &reply) else {
                continue;
            };
            let Some(reference) = crate::quotes::reference(&content) else {
                continue;
            };
            if reference["type"] != 3 {
                continue;
            }
            parsed += 1;
            for (key, value) in crate::quotes::image_reference_shape(&reference)
                .as_object()
                .unwrap()
            {
                if value == true {
                    *stages.entry(key.clone()).or_default() += 1;
                }
            }
            let mut candidate_count = 0;
            if let Ok(server) = text(&reference, "server_id").parse::<u64>() {
                let table = format!("Msg_{:x}", Md5::digest(group.as_bytes()));
                for rel in access.db.rels().into_iter().filter(|rel| {
                    rel.strip_prefix("message/message_")
                        .and_then(|name| name.strip_suffix(".db"))
                        .is_some_and(|name| {
                            !name.is_empty() && name.bytes().all(|byte| byte.is_ascii_digit())
                        })
                }) {
                    let candidates = (|| -> Result<Vec<(i64, i64)>> {
                        let connection = access.db.open(&rel)?;
                        if !connection.query_row("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1)", [&table], |row| row.get::<_,bool>(0))? { return Ok(vec![]); }
                        Ok(connection
                            .prepare(&format!(
                                "SELECT local_id,sort_seq FROM {table} WHERE server_id=?1 LIMIT 2"
                            ))?
                            .query_map([server as i64], |row| Ok((row.get(0)?, row.get(1)?)))?
                            .collect::<rusqlite::Result<Vec<_>>>()?)
                    })();
                    let Ok(candidates) = candidates else {
                        *stages.entry("candidate_read_error".into()).or_default() += 1;
                        continue;
                    };
                    for (local, sequence) in candidates {
                        candidate_count += 1;
                        *stages.entry("candidate_rows".into()).or_default() += 1;
                        let locator = json!({"local_id":local,"sort_seq":sequence,"shard":rel.replace('/',"__")});
                        let Ok(row) = access.db.reload(group, &locator) else {
                            continue;
                        };
                        let Ok(raw) = access.db.raw_message(group, local, text(&locator, "shard"))
                        else {
                            continue;
                        };
                        let diagnostic = crate::quotes::reference_diagnostics(
                            group,
                            &row,
                            &raw,
                            &access.db.wxid,
                            &reference,
                        );
                        for (key, value) in diagnostic.as_object().unwrap() {
                            if value == true {
                                *stages.entry(key.clone()).or_default() += 1;
                            }
                            if key == "source_failure" {
                                *stages
                                    .entry(format!(
                                        "source_failure_{}",
                                        value.as_str().unwrap_or("other")
                                    ))
                                    .or_default() += 1;
                            }
                        }
                    }
                }
            }
            *stages
                .entry(
                    match candidate_count {
                        0 => "references_without_candidate",
                        1 => "references_with_unique_candidate",
                        _ => "references_with_duplicate_candidates",
                    }
                    .into(),
                )
                .or_default() += 1;
            let Ok(Some(original)) = access.referenced_original(group, &reference) else {
                continue;
            };
            // referenced_original already checks the exact server ID, DB actor, locator and image MD5.
            let Ok(original_source) = source(group, &original) else {
                continue;
            };
            matched += 1;
            if access
                .media
                .inbound(&mut access.db, group, &original, "image")
                .is_ok()
            {
                media_available += 1;
            }
            if let Ok(Some(converted)) = access.convert(group, &reply) {
                if converted["_quote_source"] == original_source
                    && converted["_quote_server_id"] == reference["server_id"]
                    && converted["quote"]["id"]
                        == Uuid::new_v5(&Uuid::NAMESPACE_URL, original_source.as_bytes())
                            .to_string()
                {
                    converted_source_matched += 1;
                }
            }
        }
        ensure!(
            access.native.pid.is_none() && !disconnected.exists(),
            "Probe initialized a native component"
        );
        drop(access);
        ensure!(
            !disconnected.exists(),
            "Probe Drop changed its native directory"
        );
        Ok(
            json!({"total":identities.len(),"reloaded":reloaded,"production_classified":classified,"parsed":parsed,"target_source_matched":matched,"converted_source_matched":converted_source_matched,"media_available":media_available,"stages":stages}),
        )
    })();
    match result {
        Ok(counts) => println!("{counts}"),
        Err(_) => panic!("Read-only quote probe setup failed; private diagnostics were suppressed"),
    }
}

#[test]
fn background_recall_is_disabled_before_any_database_or_native_submission() {
    let (temp, mut access, _) = recall_db_fixture();
    access.allowed.clear();
    assert!(!access.recall_supported());
    let error = access
        .recall_info("unapproved", "invalid locator", f64::NAN)
        .unwrap_err();
    assert_eq!(
        error.to_string(),
        crate::actions::BACKGROUND_RECALL_UNAVAILABLE
    );
    let error = access
        .recall_submit("unapproved", &Value::Null, "fixture")
        .unwrap_err();
    assert!(
        error
            .downcast_ref::<crate::recall::DefinitelyNotSubmitted>()
            .is_some()
    );
    assert_eq!(
        error.to_string(),
        crate::actions::BACKGROUND_RECALL_UNAVAILABLE
    );
    assert!(access.actions.is_none());
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn recall_progress_only_observes_same_source_server_db_revoke_without_actions_or_ui() {
    let (temp, mut access, table) = recall_db_fixture();
    let info = json!({
        "server_id":"18446744073709551615",
        "locator":locator(RECALL_GROUP, &format!("wx:{RECALL_GROUP}:message__message_0.db:3:30")).unwrap()
    });
    assert_eq!(
        access
            .recall_progress(RECALL_GROUP, &info, "fixture-old-uncertain")
            .unwrap(),
        None
    );
    for (kind, body, server, expected) in [
        (
            1,
            "<sysmsg type=\"revokemsg\"><revokemsg/></sysmsg>",
            -1,
            None,
        ),
        (
            10000,
            "<sysmsg type=\"other\"><revokemsg/></sysmsg>",
            -1,
            None,
        ),
        (10000, "<sysmsg type=\"revokemsg\"/>", -1, None),
        (
            10000,
            "<sysmsg type=\"revokemsg\"><revokemsg/></sysmsg>",
            42,
            None,
        ),
        (
            10000,
            "<sysmsg type=\"revokemsg\"><revokemsg/></sysmsg>",
            -1,
            Some(true),
        ),
    ] {
        access.db.open("message/message_0.db").unwrap()
            .execute(&format!("UPDATE {table} SET local_type=?1,message_content=?2,server_id=?3 WHERE local_id=3"), params![kind,body,server]).unwrap();
        assert_eq!(
            access
                .recall_progress(RECALL_GROUP, &info, "fixture-old-uncertain")
                .unwrap(),
            expected
        );
    }
    let mut missing_server = info.clone();
    missing_server["server_id"] = json!("");
    assert_eq!(
        access
            .recall_progress(RECALL_GROUP, &missing_server, "fixture-old-uncertain")
            .unwrap(),
        None
    );
    assert!(access.actions.is_none());
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}
