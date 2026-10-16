use super::*;
use tempfile::TempDir;

#[test]
fn announcement_reads_selected_group_without_authorizing_native_or_changing_message_cursors() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.last_poll = 1000.0;
    let cursor = engine.store.state("cursor:g").unwrap();
    let floor = engine.store.state("watch_floor:g").unwrap();
    let reads = engine.access.reads;
    let events = engine.cycle(
        &[json!({"op":"announcement","group":"g","request_id":"fixture-request"})],
        1000.1,
    );
    let announcement = events
        .iter()
        .find(|event| event["type"] == "announcement")
        .unwrap();
    assert_eq!(announcement["request_id"], "fixture-request");
    assert_eq!(announcement["group"], "g");
    assert_eq!(announcement["available"], true);
    assert_eq!(announcement["set"], true);
    assert_eq!(announcement["text"], "Fixture announcement");
    assert_eq!(announcement["updated_at"], 1_760_000_000_000_i64);
    assert_eq!(engine.access.calls, vec!["announcement:g"]);
    assert!(engine.access.allowed.is_empty());
    assert!(engine.active.is_empty());
    assert_eq!(engine.access.reads, reads);
    assert_eq!(engine.store.state("cursor:g").unwrap(), cursor);
    assert_eq!(engine.store.state("watch_floor:g").unwrap(), floor);
    let idle = engine.cycle(&[], 1000.2);
    assert!(!idle.iter().any(|event| event["type"] == "announcement"));
    assert_eq!(engine.access.calls, vec!["announcement:g"]);
}

#[test]
fn announcement_unknown_unselected_revoked_and_invalid_requests_never_read() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let request = |group: &str, request: &str| json!({"op":"announcement","group":group,"request_id":request});
    for command in [
        request("h", "other"),
        request("unknown", "unknown"),
        request("g", ""),
        request("g", &"x".repeat(129)),
        request("g", "bad\nrequest"),
    ] {
        let events = engine.cycle(&[command], 1000.1);
        let reply = events
            .iter()
            .find(|event| event["type"] == "announcement")
            .unwrap();
        assert_eq!(reply["available"], false);
        assert_eq!(reply["text"], "");
        assert_eq!(reply["error"], "unavailable");
    }
    let events = engine.cycle(
        &[
            json!({"op":"configure","groups":[]}),
            request("g", "revoked"),
        ],
        1000.2,
    );
    assert!(events.iter().any(|event| event["type"] == "announcement"
        && event["available"] == false
        && event["request_id"] == "revoked"));
    assert!(engine.access.calls.is_empty());
    assert!(engine.access.allowed.is_empty());
}

#[test]
fn announcement_read_failure_is_correlated_unavailable_without_private_diagnostics_or_retry() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "announcement_error";
    let events = engine.cycle(
        &[json!({"op":"announcement","group":"g","request_id":"failure"})],
        1000.1,
    );
    let reply = events
        .iter()
        .find(|event| event["type"] == "announcement")
        .unwrap();
    assert_eq!(reply["request_id"], "failure");
    assert_eq!(reply["available"], false);
    assert_eq!(reply["error"], "unavailable");
    assert!(
        !serde_json::to_string(&events)
            .unwrap()
            .contains("private details")
    );
    engine.cycle(&[], 1001.8);
    assert_eq!(engine.access.calls, vec!["announcement:g"]);
    assert!(engine.access.allowed.is_empty());
}

#[derive(Clone)]
struct Fake {
    rows: HashMap<String, Vec<Value>>,
    calls: Vec<String>,
    allowed: BTreeSet<String>,
    reads: usize,
    signature: u64,
    mode: &'static str,
    extra_group: bool,
    media_pending: bool,
}
fn row(seq: i64, id: i64) -> Value {
    json!({"sort_seq":seq,"local_id":id,"shard":"message__message_0.db","sender_id":7,"sender_username":"member-a","type":"文本","content":"你好","server_id":seq+id,"source":"","create_time":1000})
}
impl Default for Fake {
    fn default() -> Self {
        Self {
            rows: HashMap::from([("g".to_owned(), vec![row(10, 1)])]),
            calls: vec![],
            allowed: BTreeSet::new(),
            reads: 0,
            signature: 1,
            mode: "confirm",
            extra_group: false,
            media_pending: false,
        }
    }
}
fn tuple(row: &Value) -> (i64, String, i64) {
    (
        row["sort_seq"].as_i64().unwrap(),
        text(row, "shard").to_owned(),
        row["local_id"].as_i64().unwrap(),
    )
}
impl Backend for Fake {
    fn catalog(&mut self) -> Result<Vec<Value>> {
        let mut groups = vec![
            json!({"id":"g","name":"测试群"}),
            json!({"id":"h","name":"另一个群"}),
        ];
        if self.extra_group {
            groups.push(json!({"id":"new","name":"新增群"}));
        }
        Ok(groups)
    }
    fn members(&mut self, _: &str) -> Result<Vec<Value>> {
        Ok(vec![
            json!({"id":"member-a","name":"Nick"}),
            json!({"id":"member-b","name":"Nick"}),
        ])
    }
    fn announcement(&mut self, group: &str) -> Result<Value> {
        self.calls.push(format!("announcement:{group}"));
        ensure!(
            self.mode != "announcement_error",
            "fixture read failure with private details"
        );
        Ok(
            json!({"available":true,"set":true,"author_id":"fixture-member","author":"Fixture author","avatar":"","updated_at":1_760_000_000_000_i64,"text":"Fixture announcement"}),
        )
    }
    fn recent(&mut self, g: &str, limit: usize) -> Result<Vec<Value>> {
        self.reads += 1;
        let mut rows = self.rows.get(g).cloned().unwrap_or_default();
        rows.sort_by_key(tuple);
        Ok(rows.into_iter().rev().take(limit).collect())
    }
    fn page(&mut self, g: &str, cursor: &Value, limit: usize) -> Result<Vec<Value>> {
        self.reads += 1;
        let mut rows = self.rows.get(g).cloned().unwrap_or_default();
        rows.sort_by_key(tuple);
        Ok(rows
            .into_iter()
            .filter(|r| tuple(r) > tuple(cursor))
            .take(limit)
            .collect())
    }
    fn older(&mut self, group: &str, before: Option<&Value>, limit: usize) -> Result<Vec<Value>> {
        self.reads += 1;
        let mut rows = self.rows.get(group).cloned().unwrap_or_default();
        rows.sort_by_key(|row| std::cmp::Reverse(tuple(row)));
        Ok(rows
            .into_iter()
            .filter(|row| before.is_none_or(|before| tuple(row) < tuple(before)))
            .take(limit)
            .collect())
    }
    fn reload(&mut self, g: &str, loc: &Value) -> Result<Value> {
        self.rows
            .get(g)
            .unwrap()
            .iter()
            .find(|r| {
                r["local_id"] == loc["local_id"]
                    && r["shard"] == loc["shard"]
                    && r["sort_seq"] == loc["sort_seq"]
            })
            .cloned()
            .context("原消息不在")
    }
    fn signatures(&mut self) -> Result<Value> {
        Ok(json!(self.signature))
    }
    fn authorize(&mut self, g: &BTreeSet<String>) -> Result<()> {
        self.allowed = g.clone();
        Ok(())
    }
    fn convert(&mut self, g: &str, r: &Value) -> Result<Option<Value>> {
        ensure!(
            !self.media_pending || text(r, "type") == "文本",
            "fixture media is not ready"
        );
        Ok(Some(
            json!({"id":source(g,r)?,"source_id":source(g,r)?,"group":g,"text":r["content"]}),
        ))
    }
    fn prepare_media(&mut self, _: &Path, _: &str, index: usize, m: &Value) -> Result<Step> {
        ensure!(m["bad"] != true, "附件校验失败");
        Ok(Step {
            expected: json!({"kind":"file","md5":"abcd","name":format!("{index}.txt"),"size":3,"gif_attachment":false,"video_attachment":false}),
            path: Some(PathBuf::from("fixture.txt")),
        })
    }
    fn prepare_quote(
        &mut self,
        _group: &str,
        source: &str,
        body: &str,
        atlist: &str,
    ) -> Result<Step> {
        ensure!(!source.is_empty(), "引用原文没有确认来源");
        Ok(Step {
            expected: json!({"kind":"quote","text":body,"atlist":atlist,"source_id":source}),
            path: None,
        })
    }
    fn submit(&mut self, g: &str, step: &Step, key: &str) -> Result<()> {
        self.calls.push(key.to_owned());
        if self.mode == "reject" {
            return Err(Rejected.into());
        }
        if self.mode == "lost" {
            anyhow::bail!("提交后断线");
        }
        if self.mode == "wait" {
            return Ok(());
        }
        let mut new = row(100 + self.calls.len() as i64, self.calls.len() as i64 + 100);
        new["sender_id"] = json!(if self.mode == "wrong_actor" { 7 } else { 2 });
        new["sender_username"] = json!(if self.mode == "wrong_actor" {
            "member-a"
        } else {
            "self"
        });
        new["content"] = if ["text", "quote"].contains(&text(&step.expected, "kind")) {
            step.expected["text"].clone()
        } else {
            step.expected["name"].clone()
        };
        new["native_quote"] = json!(text(&step.expected, "kind") == "quote");
        if self.mode != "wrong_at" {
            new["atlist"] = step.expected["atlist"].clone();
        }
        let entries = self.rows.entry(g.to_owned()).or_default();
        entries.push(new.clone());
        if self.mode == "duplicate" {
            new["local_id"] = json!(900);
            entries.push(new);
        }
        self.signature += 1;
        Ok(())
    }
    fn matches(&mut self, _: &str, r: &Value, e: &Value) -> Result<bool> {
        Ok(r["sender_id"] == 2
            && r["sender_username"] == "self"
            && &r["content"]
                == if ["text", "quote"].contains(&text(e, "kind")) {
                    &e["text"]
                } else {
                    &e["name"]
                }
            && (text(e, "atlist").is_empty() || r["atlist"] == e["atlist"]))
    }
    fn server_id(&mut self, _: &str, r: &Value) -> Result<String> {
        Ok(r["server_id"].to_string())
    }
    fn recall_info(&mut self, g: &str, key: &str, _: f64) -> Result<Value> {
        ensure!(
            key.starts_with(&format!("wx:{g}:")) && self.mode != "other_actor",
            "原消息不属于当前账号/群"
        );
        Ok(json!({"server_id":"42","local_id":1,"create_time":1000,"source":key}))
    }
    fn recall_submit(&mut self, _: &str, _: &Value, id: &str) -> Result<()> {
        self.calls.push(format!("recall:{id}"));
        if self.mode == "reject" {
            return Err(Rejected.into());
        }
        if self.mode == "lost" {
            anyhow::bail!("撤回响应丢失");
        }
        Ok(())
    }
    fn recall_progress(&mut self, _: &str, _: &Value, _: &str) -> Result<Option<bool>> {
        Ok(match self.mode {
            "wait" => None,
            "reject_result" => Some(false),
            _ => Some(true),
        })
    }
    fn release(&mut self) {}
}
fn setup(policy: Value) -> (TempDir, Engine<Fake>) {
    let temp = TempDir::new().unwrap();
    let engine = Engine::new(Fake::default(), temp.path().to_owned(), &policy, 1000.0).unwrap();
    (temp, engine)
}
fn message() -> Value {
    json!({"id":uuid::Uuid::new_v4().to_string(),"group":"g","text":"你好","media":[],"mentions":[]})
}
fn send(message: &Value) -> Value {
    json!({"op":"send","message":message})
}
fn ack(events: &[Value]) -> Option<&Value> {
    events.iter().find(|event| event["type"] == "ack")
}
fn pending(engine: &Engine<Fake>) -> i64 {
    engine
        .store
        .db
        .query_row("SELECT count(*) FROM pending", [], |r| r.get(0))
        .unwrap()
}

#[test]
fn explicit_history_reads_older_than_warm_40_without_moving_live_cursor_or_native_permissions() {
    let temp = TempDir::new().unwrap();
    let mut fake = Fake::default();
    fake.rows.insert(
        "g".into(),
        (1..=137).map(|index| row(index, index)).collect(),
    );
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1000.0).unwrap();
    engine.store.set("history_loaded:g", &json!(true)).unwrap();
    let live = engine.store.state("cursor:g").unwrap();
    let floor = engine.store.state("watch_floor:g").unwrap();
    let mut before = String::new();
    let mut found = BTreeSet::new();
    for page in 0..3 {
        let events=engine.cycle(&[json!({"op":"history","request_id":uuid::Uuid::new_v4().to_string(),"group":"g","before_source":before,"limit":60})],1000.1+page as f64);
        let done = events
            .iter()
            .find(|event| event["type"] == "history_done")
            .unwrap();
        assert!(done.get("error").is_none());
        assert_eq!(done["has_more"], page < 2);
        before = text(done, "before_source").to_owned();
        for message in events.iter().filter(|event| event["type"] == "message") {
            assert_eq!(message["historical"], true);
            assert!(found.insert(text(&message["message"], "source_id").to_owned()));
        }
        assert_eq!(engine.store.state("cursor:g").unwrap(), live);
        assert_eq!(engine.store.state("watch_floor:g").unwrap(), floor);
    }
    assert_eq!(found.len(), 137);
    assert!(engine.access.allowed.is_empty());
    assert!(engine.access.calls.is_empty());
    let delayed = engine.cycle(&[], 1040.0);
    assert!(
        delayed
            .iter()
            .filter(|event| event["type"] == "message")
            .all(|event| event["historical"] == true)
    );
}

#[test]
fn history_empty_visible_system_page_still_advances_and_unselected_or_cross_group_cursors_fail() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.rows.insert(
        "g".into(),
        (1..=90)
            .map(|index| {
                let mut value = row(index, index);
                value["type"] = json!("系统消息");
                value
            })
            .collect(),
    );
    let request = |group: &str, before: &str| json!({"op":"history","request_id":uuid::Uuid::new_v4().to_string(),"group":group,"before_source":before,"limit":60});
    let events = engine.cycle(&[request("g", "")], 1000.1);
    let done = events
        .iter()
        .find(|event| event["type"] == "history_done")
        .unwrap();
    assert_eq!(done["has_more"], true);
    assert!(done["sources"].as_array().unwrap().is_empty());
    assert!(!text(done, "before_source").is_empty());
    for bad in [
        request("h", ""),
        request("g", "wx:h:message__message_0.db:1:1"),
    ] {
        let events = engine.cycle(&[bad], 1000.2);
        let done = events
            .iter()
            .find(|event| event["type"] == "history_done")
            .unwrap();
        assert!(done["error"].is_string());
        assert!(done.get("has_more").is_none());
    }
    assert!(engine.access.allowed.is_empty());
}

#[test]
fn history_does_not_publish_the_unconfirmed_native_part_of_an_inflight_send() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "wait";
    let outgoing = message();
    engine.cycle(&[send(&outgoing)], 1002.0);
    let mut own = row(100, 100);
    own["sender_id"] = json!(2);
    own["sender_username"] = json!("self");
    engine.access.rows.get_mut("g").unwrap().push(own.clone());
    let events=engine.cycle(&[json!({"op":"history","request_id":uuid::Uuid::new_v4().to_string(),"group":"g","before_source":"","limit":60})],1004.0);
    let own_source = source("g", &own).unwrap();
    assert!(
        !events
            .iter()
            .any(|event| event["type"] == "message" && event["message"]["source_id"] == own_source)
    );
    assert_eq!(ack(&events).unwrap()["status"], "sent");
    assert_eq!(engine.access.calls.len(), 1);
}

#[test]
fn empty_default_never_reads_messages_or_authorizes_native() {
    let (_temp, mut engine) = setup(json!([]));
    assert_eq!(engine.access.reads, 0);
    engine.cycle(&[], 1002.0);
    assert_eq!(engine.access.reads, 0);
    assert!(engine.access.allowed.is_empty());
    let events = engine.cycle(&[json!({"op":"activate","group":"g"})], 1004.0);
    assert_eq!(events[0]["type"], "warning");
    assert!(engine.access.allowed.is_empty());
}
#[test]
fn policy_rejects_malformed_and_ignores_unknown_groups() {
    let (_temp, mut engine) = setup(json!([]));
    engine.cycle(
        &[json!({"op":"configure","groups":["g","unknown"]})],
        1002.0,
    );
    assert_eq!(engine.watched, BTreeSet::from(["g".to_owned()]));
    assert!(engine.access.allowed.is_empty());
    let events = engine.cycle(&[json!({"op":"configure","groups":"g"})], 1004.0);
    assert!(events.iter().any(|e| e["type"] == "warning"));
    assert!(engine.watched.contains("g"));
}
#[test]
fn only_selected_groups_can_activate_or_send() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    m["group"] = json!("h");
    let events = engine.cycle(&[send(&m)], 1002.0);
    assert_eq!(ack(&events).unwrap()["status"], "failed");
    assert!(engine.access.calls.is_empty());
}
#[test]
fn send_is_confirmed_and_duplicate_uuid_never_resubmits() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let m = message();
    let events = engine.cycle(&[send(&m)], 1002.0);
    assert_eq!(ack(&events).unwrap()["status"], "sent");
    assert_eq!(engine.access.calls.len(), 1);
    let again = engine.cycle(&[send(&m)], 1004.0);
    assert_eq!(ack(&again).unwrap()["status"], "sent");
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn duplicate_uuid_with_changed_body_is_rejected() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    engine.cycle(&[send(&m)], 1002.0);
    m["text"] = json!("changed");
    let result = engine.cycle(&[send(&m)], 1004.0);
    assert!(ack(&result).unwrap()["status"] != "sent");
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn actual_member_id_disambiguates_duplicate_names() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    m["text"] = json!("@Nick\u{2005}你好");
    m["mentions"] = json!([{"id":"member-b","name":"Nick"}]);
    let events = engine.cycle(&[send(&m)], 1002.0);
    assert_eq!(ack(&events).unwrap()["status"], "sent");
    let attempt = engine
        .store
        .state(&format!("wx_attempt:{}:0", text(&m, "id")))
        .unwrap()
        .unwrap();
    assert_eq!(attempt["expected"]["atlist"], "member-b");
}
#[test]
fn invalid_mentions_and_bad_final_attachment_prevent_all_submission() {
    for invalid in ["member", "attachment"] {
        let (_temp, mut engine) = setup(json!(["g"]));
        let mut m = message();
        if invalid == "member" {
            m["mentions"] = json!([{"id":"bad","name":"Nick"}]);
        } else {
            m["media"] = json!([{}, {"bad":true}]);
        }
        let result = engine.cycle(&[send(&m)], 1002.0);
        assert_eq!(ack(&result).unwrap()["status"], "failed");
        assert!(engine.access.calls.is_empty());
    }
}
#[test]
fn reject_and_lost_reply_have_distinct_terminal_status_and_no_retry() {
    for (mode, status) in [("reject", "failed"), ("lost", "uncertain")] {
        let (_temp, mut engine) = setup(json!(["g"]));
        engine.access.mode = mode;
        let m = message();
        let events = engine.cycle(&[send(&m)], 1002.0);
        assert_eq!(ack(&events).unwrap()["status"], status);
        engine.cycle(&[send(&m)], 1004.0);
        assert_eq!(engine.access.calls.len(), 1);
    }
}
#[test]
fn wrong_actor_missing_at_and_duplicate_echo_never_confirm() {
    for mode in ["wrong_actor", "wrong_at", "duplicate"] {
        let (_temp, mut engine) = setup(json!(["g"]));
        engine.access.mode = mode;
        let mut m = message();
        m["text"] = json!("@Nick你好");
        m["mentions"] = json!([{"id":"member-a","name":"Nick"}]);
        let events = engine.cycle(&[send(&m)], 1002.0);
        assert!(ack(&events).is_none_or(|ack| ack["status"] != "sent"));
        let events = engine.cycle(&[], 1034.0);
        assert!(ack(&events).is_none_or(|ack| ack["status"] != "sent"));
        assert_eq!(engine.access.calls.len(), 1);
    }
}
#[test]
fn pending_confirmations_survive_restart_without_resubmitting() {
    let (temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "wait";
    let m = message();
    assert!(ack(&engine.cycle(&[send(&m)], 1002.0)).is_none());
    let fake = engine.access.clone();
    drop(engine);
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1004.0).unwrap();
    assert!(ack(&engine.cycle(&[send(&m)], 1004.0)).is_none());
    assert_eq!(engine.access.calls.len(), 1);
    let mut echoed = row(102, 102);
    echoed["sender_id"] = json!(2);
    echoed["sender_username"] = json!("self");
    engine.access.rows.get_mut("g").unwrap().push(echoed);
    let events = engine.cycle(&[], 1006.0);
    assert_eq!(ack(&events).unwrap()["status"], "sent");
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn cancel_partial_send_never_submits_its_next_attachment() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    m["media"] = json!([{}]);
    engine.cycle(&[send(&m)], 1002.0);
    assert_eq!(engine.access.calls.len(), 1);
    let events = engine.cycle(&[json!({"op":"configure","groups":[]})], 1004.0);
    assert_eq!(ack(&events).unwrap()["status"], "uncertain");
    assert!(engine.access.allowed.is_empty());
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn cancel_pending_part_reconciles_only_that_attempt() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "wait";
    let mut m = message();
    m["media"] = json!([{}]);
    engine.cycle(&[send(&m)], 1002.0);
    assert!(ack(&engine.cycle(&[json!({"op":"configure","groups":[]})], 1004.0)).is_none());
    let events = engine.cycle(&[], 1034.0);
    assert_eq!(ack(&events).unwrap()["status"], "uncertain");
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn batched_revocation_precedes_any_native_submission() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let m = message();
    let events = engine.cycle(&[send(&m), json!({"op":"configure","groups":[]})], 1002.0);
    assert_eq!(ack(&events).unwrap()["status"], "failed");
    assert!(engine.access.calls.is_empty());
}
#[test]
fn incoming_journal_is_durable_until_matching_group_receipt() {
    let (temp, mut engine) = setup(json!(["g"]));
    engine.access.rows.get_mut("g").unwrap().push(row(11, 2));
    let events = engine.cycle(&[], 1002.0);
    let incoming = events.iter().find(|e| e["type"] == "message").unwrap()["message"].clone();
    assert_eq!(pending(&engine), 1);
    engine.cycle(
        &[json!({"op":"receipt","group":"h","source_id":incoming["source_id"]})],
        1004.0,
    );
    assert_eq!(pending(&engine), 1);
    let fake = engine.access.clone();
    drop(engine);
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1034.0).unwrap();
    assert!(
        engine
            .cycle(&[], 1034.0)
            .iter()
            .any(|e| e["type"] == "message")
    );
    engine.cycle(
        &[json!({"op":"receipt","group":"g","source_id":incoming["source_id"]})],
        1036.0,
    );
    assert_eq!(pending(&engine), 0);
    engine.access.signature += 1;
    engine.cycle(&[], 1038.0);
    assert_eq!(pending(&engine), 0);
}
#[test]
fn new_rows_at_baseline_equal_sequence_are_not_lost() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.rows.get_mut("g").unwrap().push(row(10, 2));
    let events = engine.cycle(&[], 1002.0);
    assert_eq!(pending(&engine), 1);
    assert_eq!(events.iter().filter(|e| e["type"] == "message").count(), 1);
}
#[test]
fn bounded_pages_cover_every_equal_sequence_row() {
    let (_temp, mut engine) = setup(json!(["g"]));
    for id in 2..522 {
        engine.access.rows.get_mut("g").unwrap().push(row(11, id));
    }
    for n in 0..4 {
        engine.cycle(&[], 1002.0 + n as f64 * 2.0);
    }
    assert_eq!(pending(&engine), 520);
}
#[test]
fn backfill_does_not_rewind_cursor_or_repeat_delivered_messages() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.cycle(&[json!({"op":"activate","group":"g"})], 1002.0);
    assert_eq!(engine.store.state("cursor:g").unwrap().unwrap(), 10);
    assert_eq!(pending(&engine), 1);
    engine.cycle(&[json!({"op":"activate","group":"g"})], 1004.0);
    assert_eq!(pending(&engine), 1);
}

#[test]
fn backfill_recent_history_stays_historical_across_bounded_batches_and_retries() {
    let temp = TempDir::new().unwrap();
    let mut fake = Fake::default();
    fake.rows
        .insert("g".into(), (1..=45).map(|id| row(id, id)).collect());
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1000.0).unwrap();
    let cursor = engine.store.state("cursor:g").unwrap();
    let floor = engine.store.state("watch_floor:g").unwrap();
    let first = engine.cycle(&[json!({"op":"activate","group":"g"})], 1002.0);
    let mut sources = BTreeSet::new();
    for batch in std::iter::once(first)
        .chain((1..5).map(|index| engine.cycle(&[], 1002.0 + index as f64 * 2.0)))
    {
        for event in batch.iter().filter(|event| event["type"] == "message") {
            assert_eq!(event["historical"], true);
            assert!(sources.insert(text(&event["message"], "source_id").to_owned()));
        }
    }
    assert_eq!(sources.len(), 40);
    assert_eq!(pending(&engine), 40);
    assert_eq!(engine.store.state("cursor:g").unwrap(), cursor);
    assert_eq!(engine.store.state("watch_floor:g").unwrap(), floor);
    let retry = engine.cycle(&[], 1034.0);
    let messages: Vec<_> = retry
        .iter()
        .filter(|event| event["type"] == "message")
        .collect();
    assert!(!messages.is_empty());
    assert!(messages.iter().all(|event| event["historical"] == true));
    assert!(engine.access.calls.is_empty());
}

#[test]
fn delayed_backfill_media_preserves_historical_origin_after_restart() {
    let temp = TempDir::new().unwrap();
    let mut fake = Fake::default();
    let mut old = row(10, 1);
    old["type"] = json!("文件");
    fake.rows.insert("g".into(), vec![old.clone()]);
    fake.media_pending = true;
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1000.0).unwrap();
    let first = engine.cycle(&[json!({"op":"activate","group":"g"})], 1002.0);
    assert!(!first.iter().any(|event| event["type"] == "message"));
    assert_eq!(pending(&engine), 1);
    let fake = engine.access.clone();
    drop(engine);
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1003.0).unwrap();
    let locator: String = engine
        .store
        .db
        .query_row("SELECT locator FROM pending", [], |row| row.get(0))
        .unwrap();
    assert_eq!(
        serde_json::from_str::<Value>(&locator).unwrap()["historical"],
        true
    );
    let delayed = engine.cycle(&[json!({"op":"activate","group":"g"})], 1034.0);
    assert!(!delayed.iter().any(|event| event["type"] == "message"));
    assert_eq!(pending(&engine), 1);
    engine.access.media_pending = false;
    let ready = engine.cycle(&[], 1066.0);
    let messages: Vec<_> = ready
        .iter()
        .filter(|event| event["type"] == "message")
        .collect();
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0]["historical"], true);
    assert_eq!(
        messages[0]["message"]["source_id"],
        source("g", &old).unwrap()
    );
    assert!(engine.access.calls.is_empty());
}

#[test]
fn backfill_does_not_reclassify_an_existing_live_pending_row_at_the_baseline_sequence() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let live = row(10, 2);
    engine.access.rows.get_mut("g").unwrap().push(live.clone());
    let first = engine.cycle(&[], 1002.0);
    let live_source = source("g", &live).unwrap();
    let emitted = first
        .iter()
        .find(|event| event["type"] == "message")
        .unwrap();
    assert_eq!(emitted["historical"], false);
    assert_eq!(emitted["message"]["source_id"], live_source);
    let before: String = engine
        .store
        .db
        .query_row(
            "SELECT locator FROM pending WHERE source=?",
            [&live_source],
            |row| row.get(0),
        )
        .unwrap();
    engine.cycle(&[json!({"op":"activate","group":"g"})], 1004.0);
    let after: String = engine
        .store
        .db
        .query_row(
            "SELECT locator FROM pending WHERE source=?",
            [&live_source],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(before, after);
    let retry = engine.cycle(&[], 1034.0);
    let live_event = retry
        .iter()
        .find(|event| event["type"] == "message" && event["message"]["source_id"] == live_source)
        .unwrap();
    assert_eq!(live_event["historical"], false);
    assert!(engine.access.calls.is_empty());
}
#[test]
fn catalog_discovery_never_selects_a_new_group() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.extra_group = true;
    engine.cycle(&[], 1062.0);
    assert_eq!(engine.catalog.len(), 3);
    assert_eq!(engine.watched, BTreeSet::from(["g".to_owned()]));
}
#[test]
fn empty_group_is_watchable_before_its_first_message() {
    let (_temp, mut engine) = setup(json!(["h"]));
    engine.access.rows.insert("h".to_owned(), vec![row(1, 1)]);
    assert!(
        engine
            .cycle(&[], 1002.0)
            .iter()
            .any(|e| e["type"] == "message")
    );
}
#[test]
fn shutdown_revokes_native_access() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.cycle(&[json!({"op":"activate","group":"g"})], 1002.0);
    assert!(!engine.access.allowed.is_empty());
    engine.shutdown().unwrap();
    assert!(engine.access.allowed.is_empty());
}

#[test]
fn legacy_python_receipt_hash_keeps_utf8_spacing_and_deduplicates() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let m = message();
    let expected = json!({"group":"g","text":"你好","media":[],"mentions":[]});
    let legacy = "4ef5268eadba8208efe224cce29ab6fc91557de9cf2a6dcc09fda7b657af0cd0";
    assert_eq!(hash(&expected, true).unwrap(), legacy);
    engine
        .store
        .set(
            &format!("outgoing:{}", text(&m, "id")),
            &json!({"hash":legacy,"ack":{"type":"ack","id":m["id"],"status":"sent"}}),
        )
        .unwrap();
    assert_eq!(
        ack(&engine.cycle(&[send(&m)], 1002.0)).unwrap()["status"],
        "sent"
    );
    assert!(engine.access.calls.is_empty());
}
#[test]
fn journal_write_failure_does_not_discard_any_other_group_job() {
    let (_temp, mut engine) = setup(json!(["g", "h"]));
    engine.access.mode = "wait";
    let m = message();
    let mut other = message();
    other["group"] = json!("h");
    engine.cycle(&[send(&m), send(&other)], 1002.0);
    assert_eq!(engine.jobs.len(), 2);
    engine.store.db.execute_batch("CREATE TRIGGER reject_state BEFORE INSERT ON state BEGIN SELECT RAISE(ABORT,'fixture write error'); END;").unwrap();
    engine.cycle(&[json!({"op":"configure","groups":["h"]})], 1004.0);
    assert_eq!(engine.jobs.len(), 2);
    assert!(!engine.watched.contains("g"));
    assert!(!engine.access.allowed.contains("g"));
    engine
        .store
        .db
        .execute_batch("DROP TRIGGER reject_state")
        .unwrap();
    engine.cycle(&[json!({"op":"configure","groups":["h"]})], 1034.0);
    assert!(
        engine
            .jobs
            .iter()
            .any(|job| text(&job.message, "group") == "h")
    );
}

#[test]
fn quote_is_preserved_in_hash_and_uses_a_native_quote_step_without_summary_text() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    m["quote"] = json!({"id":"source-id","sender":"Nick","text":"原话"});
    m["_quote_source"] = json!("wx:g:message__message_0.db:1:10");
    let events = engine.cycle(&[send(&m)], 1002.0);
    assert_eq!(ack(&events).unwrap()["status"], "sent");
    let content = text(engine.access.rows["g"].last().unwrap(), "content");
    assert_eq!(content, "你好");
    assert_eq!(
        engine.access.rows["g"].last().unwrap()["native_quote"],
        true
    );
    m["quote"]["text"] = json!("伪造另一段");
    let result = engine.cycle(&[send(&m)], 1004.0);
    assert!(ack(&result).unwrap()["status"] != "sent");
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn quoted_body_alone_cannot_create_an_actual_mention() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = message();
    m["quote"] = json!({"id":"source-id","sender":"Nick","text":"@Nick"});
    m["_quote_source"] = json!("wx:g:message__message_0.db:1:10");
    m["mentions"] = json!([{"id":"member-a","name":"Nick"}]);
    assert_eq!(
        ack(&engine.cycle(&[send(&m)], 1002.0)).unwrap()["status"],
        "failed"
    );
    assert!(engine.access.calls.is_empty());
}
fn recalled_message() -> Value {
    let mut m = message();
    m["status"] = json!("sent");
    m["outgoing"] = json!(true);
    m["timestamp"] = json!(1_000_000);
    m["source_id"] = json!("wx:g:message__message_0.db:1:10");
    m
}
#[test]
fn recall_requires_backend_confirmation_and_is_idempotent() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let m = recalled_message();
    let cmd = json!({"op":"recall","message":m});
    let result = engine.cycle(&[cmd.clone()], 1002.0);
    assert!(
        result
            .iter()
            .any(|e| e["type"] == "recall_ack" && e["status"] == "recalled")
    );
    assert_eq!(engine.access.calls.len(), 1);
    engine.cycle(&[cmd], 1004.0);
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn pending_recall_checks_after_restart_never_resubmit() {
    let (temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "wait";
    let m = recalled_message();
    engine.cycle(&[json!({"op":"recall","message":m})], 1002.0);
    assert_eq!(engine.access.calls.len(), 1);
    let mut fake = engine.access.clone();
    fake.mode = "confirm";
    drop(engine);
    let mut engine = Engine::new(fake, temp.path().to_owned(), &json!(["g"]), 1004.0).unwrap();
    let result = engine.cycle(&[json!({"op":"recall_check","message":m})], 1004.0);
    assert!(
        result
            .iter()
            .any(|e| e["type"] == "recall_ack" && e["status"] == "recalled")
    );
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn missing_recall_receipt_check_never_submits() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let result = engine.cycle(
        &[json!({"op":"recall_check","message":recalled_message()})],
        1002.0,
    );
    assert!(
        result
            .iter()
            .any(|e| e["type"] == "recall_ack" && e["status"] == "failed")
    );
    assert!(engine.access.calls.is_empty());
}
#[test]
fn recall_old_future_other_account_or_unconfirmed_targets_never_submit() {
    for change in [
        json!({"timestamp":700_000}),
        json!({"timestamp":1_100_000}),
        json!({"outgoing":false}),
        json!({"source_id":""}),
        json!({"source_id":"wx:h:message__message_0.db:1:10"}),
    ] {
        let (_temp, mut engine) = setup(json!(["g"]));
        let mut m = recalled_message();
        for (k, v) in change.as_object().unwrap() {
            m[k] = v.clone();
        }
        let result = engine.cycle(&[json!({"op":"recall","message":m})], 1002.0);
        assert!(
            result
                .iter()
                .any(|e| e["type"] == "recall_ack" && e["status"] == "failed")
        );
        assert!(engine.access.calls.is_empty());
    }
}
#[test]
fn recall_partial_parts_stop_on_group_revocation() {
    let (_temp, mut engine) = setup(json!(["g"]));
    let mut m = recalled_message();
    m["source_ids"] = json!([m["source_id"], "wx:g:message__message_0.db:2:11"]);
    engine.cycle(&[json!({"op":"recall","message":m})], 1002.0);
    assert_eq!(engine.access.calls.len(), 1);
    let result = engine.cycle(&[json!({"op":"configure","groups":[]})], 1004.0);
    assert!(
        result
            .iter()
            .any(|e| e["type"] == "recall_ack" && e["status"] == "uncertain")
    );
    assert_eq!(engine.access.calls.len(), 1);
}
#[test]
fn recall_lost_response_is_uncertain_and_never_resubmits() {
    let (_temp, mut engine) = setup(json!(["g"]));
    engine.access.mode = "lost";
    let m = recalled_message();
    let cmd = json!({"op":"recall","message":m});
    let result = engine.cycle(&[cmd.clone()], 1002.0);
    assert!(
        result
            .iter()
            .any(|e| e["type"] == "recall_ack" && e["status"] == "uncertain")
    );
    engine.cycle(&[cmd], 1004.0);
    assert_eq!(engine.access.calls.len(), 1);
}
