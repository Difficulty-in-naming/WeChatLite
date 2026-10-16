use super::super::*;
use sha2::{Digest, Sha256};

pub async fn access_key_is_required_for_messages_and_files() -> Result<()> {
    let relay = Relay::new("demo").await?;
    for path in [
        "/api/groups",
        "/api/messages?group=demo-design",
        "/api/media/missing",
    ] {
        relay.api.deny(path, None, Some("wrong"), 401).await?;
    }
    Ok(())
}
pub async fn two_clients_receive_identical_confirmed_message() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let mut a = relay.api.ws(0, None).await?;
    let mut b = relay.api.ws(0, None).await?;
    sync(&mut a).await?;
    sync(&mut b).await?;
    let mut value = message("demo-design", "@林小满 需要确认一下这份视频");
    value["mentions"] = json!([{"id":"lin","name":"林小满"}]);
    let response = relay.api.post("/api/messages", value.clone()).await?;
    assert_eq!(response["id"], value["id"]);
    let first = sent(&mut a, value["id"].as_str().unwrap()).await?;
    let second = sent(&mut b, value["id"].as_str().unwrap()).await?;
    assert_eq!(first, second);
    assert_eq!(first.last().unwrap()["message"]["mentions"][0]["id"], "lin");
    Ok(())
}
pub async fn retry_is_idempotent_but_changed_content_is_rejected() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let mut value = message("demo-design", "本机同步测试");
    let first = relay.api.post("/api/messages", value.clone()).await?;
    let second = relay.api.post("/api/messages", value.clone()).await?;
    assert_eq!(first["seq"], second["seq"]);
    assert_eq!(
        relay
            .rows("demo-design")
            .await?
            .as_array()
            .unwrap()
            .iter()
            .filter(|m| m["id"] == value["id"])
            .count(),
        1
    );
    value["text"] = json!("changed");
    relay
        .api
        .deny("/api/messages", Some(value), None, 400)
        .await
}
pub async fn mention_must_match_actual_member_and_visible_text() -> Result<()> {
    let relay = Relay::new("demo").await?;
    for (mention, text) in [
        (json!({"id":"unknown","name":"林小满"}), "@林小满"),
        (json!({"id":"lin","name":"错误名字"}), "@错误名字"),
        (json!({"id":"lin","name":"林小满"}), "没有提及"),
    ] {
        let mut value = message("demo-design", text);
        value["mentions"] = json!([mention]);
        relay
            .api
            .deny("/api/messages", Some(value), None, 400)
            .await?;
    }
    Ok(())
}
pub async fn file_bytes_names_and_date_folders() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let mut raw = b"GIF89a".to_vec();
    raw.extend((0..2 * 1024 * 1024).map(|n| (n % 251) as u8));
    let media = relay
        .api
        .upload("demo-design", "../../原始动画.gif", raw.clone(), None)
        .await?;
    assert_eq!(media["name"], "原始动画.gif");
    assert_eq!(media["kind"], "gif");
    assert!(media.get("path").is_none());
    assert_eq!(media["sha256"], format!("{:x}", Sha256::digest(&raw)));
    assert_eq!(
        relay
            .api
            .download(media["id"].as_str().unwrap(), None)
            .await?,
        raw
    );
    let mut files = Vec::new();
    for entry in std::fs::read_dir(relay.dir.path().join("state/media"))? {
        let entry = entry?;
        let day = entry.file_name().to_string_lossy().into_owned();
        assert!(chrono::NaiveDate::parse_from_str(&day, "%Y-%m-%d").is_ok());
        for file in std::fs::read_dir(entry.path())? {
            let path = file?.path();
            if path
                .file_name()
                .unwrap()
                .to_string_lossy()
                .ends_with("原始动画.gif")
            {
                files.push(path);
            }
        }
    }
    assert_eq!(files.len(), 1);
    let mut value = message("demo-design", "文件测试");
    value["media"] = json!([media["id"]]);
    relay.api.post("/api/messages", value).await?;
    let history = relay.rows("demo-design").await?;
    assert!(history.as_array().unwrap().iter().any(|r| {
        r["media"]
            .as_array()
            .unwrap()
            .iter()
            .any(|m| m["id"] == media["id"])
    }));
    assert!(!history.to_string().contains("\"path\""));
    Ok(())
}
pub async fn reconnect_replays_missed_events_in_order() -> Result<()> {
    let relay = Relay::new("demo").await?;
    let first = message("demo-design", "在线消息");
    let mut socket = relay.api.ws(0, None).await?;
    sync(&mut socket).await?;
    relay.api.post("/api/messages", first.clone()).await?;
    let cursor = sent(&mut socket, first["id"].as_str().unwrap())
        .await?
        .last()
        .unwrap()["seq"]
        .as_i64()
        .unwrap();
    drop(socket);
    let second = message("demo-design", "断线期间的消息");
    relay.api.post("/api/messages", second.clone()).await?;
    let mut socket = relay.api.ws(cursor, None).await?;
    let events = sync(&mut socket).await?;
    let messages = events
        .iter()
        .filter(|e| e["type"] == "message")
        .collect::<Vec<_>>();
    assert!(messages.iter().any(|e| e["message"]["id"] == second["id"]));
    let seq = messages
        .iter()
        .map(|e| e["seq"].as_i64().unwrap())
        .collect::<Vec<_>>();
    assert!(seq.windows(2).all(|w| w[0] < w[1]));
    assert!(seq.iter().all(|n| *n > cursor));
    Ok(())
}
