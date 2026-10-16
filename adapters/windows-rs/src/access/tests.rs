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

fn recall_fixture() -> (TempDir, Local, String) {
    let temp = TempDir::new().unwrap();
    let contact = Connection::open_in_memory().unwrap();
    contact.execute_batch("CREATE TABLE contact(id INTEGER PRIMARY KEY,username TEXT,remark TEXT,nick_name TEXT,is_in_chat_room INTEGER,delete_flag INTEGER);CREATE TABLE chat_room(id INTEGER PRIMARY KEY,username TEXT,owner TEXT,ext_buffer BLOB);CREATE TABLE chatroom_member(room_id INTEGER,member_id INTEGER);INSERT INTO contact VALUES(1,'123456@chatroom','','Fixture group',1,0),(2,'wxid_peer','','Peer',0,0);INSERT INTO chat_room VALUES(1,'123456@chatroom','wxid_self',NULL);").unwrap();
    let messages = Connection::open_in_memory().unwrap();
    let table = format!("Msg_{:x}", Md5::digest(RECALL_GROUP.as_bytes()));
    messages.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);INSERT INTO Name2Id(rowid,user_name) VALUES(7,'wxid_peer');CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content TEXT,source BLOB,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);")).unwrap();
    for (id, kind, sender, body) in [
        (1, 1, 7, "wxid_peer:\nEarlier"),
        (2, 3, 7, "image"),
        (3, 1, 2, "wxid_self:\ntarget body"),
        (4, 1, 2, "Later"),
        (5, 49, 2, "<msg><appmsg><type>57</type></appmsg></msg>"),
        (6, 10000, 2, "<sysmsg type=\"other\"/>"),
    ] {
        messages
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,?2,?3,1000,?4,NULL,NULL,NULL,-1,?1*10)"),
                params![id, kind, sender, body],
            )
            .unwrap();
    }
    let extra = Connection::open_in_memory().unwrap();
    extra.execute_batch(&format!("CREATE TABLE Name2Id(user_name TEXT);INSERT INTO Name2Id(rowid,user_name) VALUES(7,'wxid_peer');CREATE TABLE {table}(local_id INTEGER PRIMARY KEY,local_type INTEGER,real_sender_id INTEGER,create_time INTEGER,message_content TEXT,source BLOB,packed_info_data BLOB,compress_content BLOB,server_id INTEGER,sort_seq INTEGER);")).unwrap();
    let access = Local {
        db: Db::media_fixture(
            temp.path().join("fixture-account"),
            "wxid_self",
            None,
            HashMap::from([
                ("contact/contact.db".into(), contact),
                ("message/message_0.db".into(), messages),
                ("message/message_1.db".into(), extra),
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
    };
    (temp, access, table)
}

fn recall_key() -> String {
    format!("wx:{RECALL_GROUP}:message__message_0.db:3:30")
}

#[test]
fn recall_snapshot_uses_confirmed_plaintext_and_unique_db_target_in_native_order() {
    let (temp, mut access, _) = recall_fixture();
    let info = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
    assert_eq!(info["group"], RECALL_GROUP);
    assert_eq!(info["group_name"], "Fixture group");
    assert_eq!(info["ui_text"], "target body");
    assert_eq!(info["server_id"], "18446744073709551615");
    assert_eq!(
        info["ui_context"],
        json!([
            {"text":"Earlier","outgoing":false,"target":false},
            {"text":"target body","outgoing":true,"target":true},
            {"text":"Later","outgoing":true,"target":false},
        ])
    );
    assert!(info.get("ui_account_gate").is_none());
    assert!(access.actions.is_none());
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

#[test]
fn recall_preparation_persists_only_target_fingerprint_not_chat_text_or_ui_profiles() {
    let (_temp, mut access, _) = recall_fixture();
    let prepared = access
        .recall_prepare(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
    assert_eq!(
        prepared["ui_text_hash"],
        hex::encode(Sha256::digest(b"target body"))
    );
    for field in [
        "ui_text",
        "ui_context",
        "group_name",
        "ui_account_gate",
        "ui_history_gate",
        "ui_unique_contexts",
    ] {
        assert!(prepared.get(field).is_none(), "{field}");
    }
    assert!(!prepared.to_string().contains("target body"));
    let fresh = access
        .recall_refresh(RECALL_GROUP, &prepared, 1002.0)
        .unwrap();
    assert_eq!(fresh["ui_text"], "target body");
    let mut legacy = prepared;
    legacy.as_object_mut().unwrap().remove("ui_text_hash");
    assert!(
        access
            .recall_refresh(RECALL_GROUP, &legacy, 1002.0)
            .is_err()
    );
}

#[test]
fn recall_refresh_reads_current_content_owner_confirmation_time_and_source_before_submit() {
    for change in [
        "message_content='changed'",
        "real_sender_id=7",
        "server_id=0",
        "server_id=42",
        "create_time=1001",
        "sort_seq=31",
        "local_type=49",
        "local_type=10000,message_content='<sysmsg type=\"revokemsg\"><revokemsg/></sysmsg>'",
    ] {
        let (_temp, mut access, table) = recall_fixture();
        let info = access
            .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
            .unwrap();
        access
            .db
            .open("message/message_0.db")
            .unwrap()
            .execute(&format!("UPDATE {table} SET {change} WHERE local_id=3"), [])
            .unwrap();
        assert!(
            access.recall_refresh(RECALL_GROUP, &info, 1002.0).is_err(),
            "{change}"
        );
        assert!(access.actions.is_none());
        assert!(access.native.pid.is_none());
    }
    let (_temp, mut access, _) = recall_fixture();
    let info = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
    assert!(access.recall_refresh(RECALL_GROUP, &info, 1180.0).is_err());
    access.allowed.clear();
    assert!(access.recall_refresh(RECALL_GROUP, &info, 1002.0).is_err());
}

#[test]
fn recall_refresh_rebuilds_history_and_current_unique_group_name() {
    let (_temp, mut access, table) = recall_fixture();
    let mut info = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
    // Persisted UI context is never used as current UI evidence.
    info["ui_context"] = json!([{"text":"stale","outgoing":true,"target":true}]);
    access
        .db
        .open("contact/contact.db")
        .unwrap()
        .execute(
            "UPDATE contact SET nick_name='Renamed group' WHERE id=1",
            [],
        )
        .unwrap();
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(7,1,7,1001,'New incoming',NULL,NULL,NULL,88,70)"),
            [],
        )
        .unwrap();
    let fresh = access.recall_refresh(RECALL_GROUP, &info, 1002.0).unwrap();
    assert_eq!(fresh["group_name"], "Renamed group");
    assert_eq!(
        fresh["ui_context"].as_array().unwrap().last().unwrap()["text"],
        "New incoming"
    );
    access
        .db
        .open("contact/contact.db")
        .unwrap()
        .execute(
            "INSERT INTO contact VALUES(3,'654321@chatroom','','Renamed group',1,0)",
            [],
        )
        .unwrap();
    assert!(access.recall_refresh(RECALL_GROUP, &info, 1002.0).is_err());
}

#[test]
fn recall_text_cross_checks_raw_type_and_actor_instead_of_display_labels_or_body_prefixes() {
    let (_temp, mut access, _) = recall_fixture();
    let locator = locator(RECALL_GROUP, &recall_key()).unwrap();
    let row = access.db.reload(RECALL_GROUP, &locator).unwrap();
    let raw = access
        .db
        .raw_message(RECALL_GROUP, 3, "message__message_0.db")
        .unwrap();
    for (field, changed) in [
        ("local_type", json!(3)),
        ("local_type", json!(0x100000001_i64)),
        ("sender_username", json!("wxid_other")),
        ("sender_id", json!(7)),
        ("sort_seq", json!(31)),
        ("create_time", json!(1001)),
        ("content", json!("wxid_other:\nchanged")),
    ] {
        let mut changed_raw = raw.clone();
        changed_raw[field] = changed;
        assert!(
            recall_target(RECALL_GROUP, &row, &changed_raw, "wxid_self", 1001.0).is_err(),
            "{field}"
        );
    }
    let mut incoming = row.clone();
    let mut incoming_raw = raw.clone();
    for value in [&mut incoming, &mut incoming_raw] {
        value["sender_id"] = json!(7);
        value["sender_username"] = json!("wxid_peer");
        value["content"] = json!("wxid_self:\nspoofed own envelope");
    }
    assert!(recall_target(RECALL_GROUP, &incoming, &incoming_raw, "wxid_self", 1001.0).is_err());
}

#[test]
fn recall_time_never_accepts_future_expired_invalid_or_non_finite_clock_values() {
    for now in [999.0, 1180.0, f64::INFINITY, f64::NEG_INFINITY, f64::NAN] {
        assert!(recall_time(&json!(1000), now).is_err());
    }
    for stamp in [Value::Null, json!("1000"), json!(0), json!(-1)] {
        assert!(recall_time(&stamp, 1001.0).is_err());
    }
    recall_time(&json!(1000), 1000.0).unwrap();
    recall_time(&json!(1000), 1179.999).unwrap();
}

fn context_text(sequence: i64, shard: &str, id: i64, body: &str, outgoing: bool) -> RecallText {
    RecallText {
        cursor: (sequence, shard.into(), id),
        text: body.into(),
        outgoing,
        break_before: false,
    }
}

#[test]
fn recall_context_sorts_exact_sources_and_never_chooses_by_repeated_text() {
    let target = json!({"sort_seq":2,"shard":"b","local_id":9});
    let context = recall_context(
        vec![
            context_text(2, "b", 9, "same text", true),
            context_text(2, "b", 10, "last", true),
            context_text(2, "a", 10, "same text", true),
            context_text(1, "b", 20, "first", false),
        ],
        &target,
        "same text",
    )
    .unwrap();
    assert_eq!(
        context,
        json!([
            {"text":"first","outgoing":false,"target":false},
            {"text":"same text","outgoing":true,"target":false},
            {"text":"same text","outgoing":true,"target":true},
            {"text":"last","outgoing":true,"target":false},
        ])
        .as_array()
        .unwrap()
        .to_owned()
    );
    for entries in [
        vec![context_text(2, "a", 9, "same text", true)],
        vec![context_text(2, "b", 9, "changed", true)],
        vec![context_text(2, "b", 9, "same text", false)],
        vec![
            context_text(2, "b", 9, "same text", true),
            context_text(2, "b", 9, "same text", true),
        ],
    ] {
        assert!(recall_context(entries, &target, "same text").is_err());
    }
    let entries = (0..201)
        .map(|id| context_text(id, "b", id, "body", true))
        .collect();
    let dropped = json!({"sort_seq":0,"shard":"b","local_id":0});
    assert!(recall_context(entries, &dropped, "body").is_err());
}

#[test]
fn recall_context_keeps_only_newest_text_within_utf8_byte_and_message_limits() {
    let large = "汉".repeat(UI_CONTEXT_BYTES / 6);
    let target = json!({"sort_seq":3,"shard":"b","local_id":3});
    let context = recall_context(
        vec![
            context_text(1, "b", 1, &large, true),
            context_text(2, "b", 2, &large, false),
            context_text(3, "b", 3, "target", true),
        ],
        &target,
        "target",
    )
    .unwrap();
    assert_eq!(context.len(), 2);
    assert!(
        context
            .iter()
            .map(|entry| text(entry, "text").len())
            .sum::<usize>()
            <= UI_CONTEXT_BYTES
    );
    assert_eq!(context[1]["target"], true);
    let oldest = json!({"sort_seq":1,"shard":"b","local_id":1});
    let newest = "汉".repeat(UI_CONTEXT_BYTES / 3 + 1);
    assert!(
        recall_context(
            vec![context_text(1, "b", 1, &newest, true)],
            &oldest,
            &newest
        )
        .is_err()
    );
}

#[test]
fn recall_group_name_requires_unique_membership_across_all_local_groups() {
    let target = json!({"username":RECALL_GROUP,"name":"Fixture group"});
    assert_eq!(
        recall_group_name(&[target.clone()], RECALL_GROUP).unwrap(),
        "Fixture group"
    );
    for groups in [
        vec![],
        vec![target.clone(), target.clone()],
        vec![
            target.clone(),
            json!({"username":"654321@chatroom","name":"Fixture group"}),
        ],
        vec![json!({"username":RECALL_GROUP,"name":" "})],
    ] {
        assert!(recall_group_name(&groups, RECALL_GROUP).is_err());
    }
}

#[test]
fn recall_progress_only_observes_same_source_server_db_revoke_without_actions_or_ui() {
    let (temp, mut access, table) = recall_fixture();
    let info = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
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
    assert!(access.actions.is_none());
    assert!(access.native.pid.is_none());
    assert!(!temp.path().join("native").exists());
}

fn proof_row(sequence: i64, body: &str, outgoing: bool) -> Value {
    json!({"sort_seq":sequence,"shard":"message__message_0.db","local_id":sequence,
        "is_text":true,"local_type":1,"content":body,
        "sender_id":if outgoing {2} else {7},"sender_username":if outgoing {"wxid_self"} else {"wxid_peer"}})
}

#[test]
fn recall_global_proof_accepts_first_unique_text_but_never_an_unfinished_prefix() {
    let context = vec![json!({"text":"First unique message","target":true,"outgoing":true})];
    let target = json!({"sort_seq":1,"shard":"message__message_0.db","local_id":1});
    for complete in [false, true] {
        let mut proof = RecallUniqueness::new(&context, &target).unwrap();
        proof
            .observe(
                RECALL_GROUP,
                &proof_row(1, "First unique message", true),
                "wxid_self",
            )
            .unwrap();
        let result = proof.finish(complete);
        if complete {
            assert_eq!(result.unwrap(), vec![json!({"start":0,"length":1})]);
        } else {
            assert!(result.is_err());
        }
    }
}

#[test]
fn recall_global_proof_uses_shortest_unique_context_and_ignores_old_senders() {
    let context = vec![
        json!({"text":"A","target":false}),
        json!({"text":"T","target":true}),
        json!({"text":"B","target":false}),
    ];
    let target = json!({"sort_seq":5,"shard":"message__message_0.db","local_id":5});
    let mut proof = RecallUniqueness::new(&context, &target).unwrap();
    for (sequence, body) in ["X", "T", "Y", "A", "T", "B"].into_iter().enumerate() {
        proof
            .observe(
                RECALL_GROUP,
                &proof_row(sequence as i64 + 1, body, sequence >= 3),
                "wxid_self",
            )
            .unwrap();
    }
    assert_eq!(
        proof.finish(true).unwrap(),
        vec![json!({"start":0,"length":2}), json!({"start":1,"length":2})]
    );
    let target = json!({"sort_seq":8,"shard":"message__message_0.db","local_id":8});
    let mut proof = RecallUniqueness::new(&context, &target).unwrap();
    for (sequence, body) in ["C", "A", "T", "B", "D", "C", "A", "T", "B", "D"]
        .into_iter()
        .enumerate()
    {
        proof
            .observe(
                RECALL_GROUP,
                &proof_row(sequence as i64 + 1, body, sequence >= 5),
                "wxid_self",
            )
            .unwrap();
    }
    assert!(proof.finish(true).is_err());
}

#[test]
fn recall_global_proof_never_crosses_breaks_or_skips_unknown_type1_and_repeated_cursors() {
    let context = vec![
        json!({"text":"A","target":false}),
        json!({"text":"T","target":true,"break_before":true}),
        json!({"text":"B","target":false,"break_before":true}),
    ];
    let target = json!({"sort_seq":5,"shard":"message__message_0.db","local_id":5});
    let mut proof = RecallUniqueness::new(&context, &target).unwrap();
    for (sequence, body) in ["X", "T", "Y", "A", "T", "B"].into_iter().enumerate() {
        proof
            .observe(
                RECALL_GROUP,
                &proof_row(sequence as i64 + 1, body, true),
                "wxid_self",
            )
            .unwrap();
    }
    assert!(proof.finish(true).is_err());
    let mut proof = RecallUniqueness::new(&context, &target).unwrap();
    let mut bad = proof_row(1, "T", true);
    bad.as_object_mut().unwrap().remove("content");
    assert!(proof.observe(RECALL_GROUP, &bad, "wxid_self").is_err());
    let mut proof = RecallUniqueness::new(&context, &target).unwrap();
    let row = proof_row(1, "A", true);
    proof.observe(RECALL_GROUP, &row, "wxid_self").unwrap();
    assert!(proof.observe(RECALL_GROUP, &row, "wxid_self").is_err());
}

#[test]
fn recall_full_scan_finds_old_duplicates_across_pages_and_shards_outside_latest_200() {
    let (_temp, mut access, table) = recall_fixture();
    for (id, seq, body) in [(8, 5, "Before"), (9, 45, "After")] {
        access
            .db
            .open("message/message_0.db")
            .unwrap()
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,1,2,1000,?2,NULL,NULL,NULL,42,?3)"),
                params![id, body, seq],
            )
            .unwrap();
    }
    for index in 1..=410_i64 {
        let body = match index {
            199 => "Before".into(),
            200 => "Earlier".into(),
            201 => "target body".into(),
            202 => "Later".into(),
            203 => "After".into(),
            _ => format!("Older filler {index}"),
        };
        let rel = if index % 2 == 0 {
            "message/message_0.db"
        } else {
            "message/message_1.db"
        };
        access
            .db
            .open(rel)
            .unwrap()
            .execute(
                &format!("INSERT INTO {table} VALUES(?1,1,7,1000,?2,NULL,NULL,NULL,42,?3)"),
                params![index + 1000, body, index - 10000],
            )
            .unwrap();
    }
    let error = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap_err();
    assert!(error.to_string().contains("不唯一"), "{error}");
    assert!(access.actions.is_none());
}

#[test]
fn recall_partial_or_timed_out_scan_and_unknown_old_text_never_prove_uniqueness() {
    let (_temp, mut access, table) = recall_fixture();
    let info = access
        .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
        .unwrap();
    let snapshot = access.db.recall_history_snapshot().unwrap();
    for limits in [
        RecallScanLimits {
            rows: 4,
            duration: Duration::from_secs(10),
        },
        RecallScanLimits {
            rows: 200000,
            duration: Duration::ZERO,
        },
    ] {
        assert!(
            access
                .recall_unique_contexts(
                    RECALL_GROUP,
                    &snapshot,
                    info["ui_context"].as_array().unwrap(),
                    &info["locator"],
                    Instant::now(),
                    limits
                )
                .is_err()
        );
    }
    access
        .db
        .open("message/message_0.db")
        .unwrap()
        .execute(
            &format!("INSERT INTO {table} VALUES(100,1,7,900,x'ff00',NULL,NULL,NULL,42,-100)"),
            [],
        )
        .unwrap();
    assert!(
        access
            .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
            .is_err()
    );
}

#[test]
fn recall_same_name_personal_contact_is_rejected_even_when_group_names_are_unique() {
    let (_temp, mut access, _) = recall_fixture();
    access
        .db
        .open("contact/contact.db")
        .unwrap()
        .execute(
            "UPDATE contact SET nick_name='Fixture group' WHERE username='wxid_peer'",
            [],
        )
        .unwrap();
    assert!(
        access
            .recall_snapshot(RECALL_GROUP, &recall_key(), 1001.0)
            .unwrap_err()
            .to_string()
            .contains("同名联系人")
    );
}
