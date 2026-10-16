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
    };
    let rows = access
        .db
        .page(GROUP, &json!({"sort_seq":0,"shard":"","local_id":0}), 2)
        .unwrap();
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
