use super::super::*;

fn path(before: &str, filters: &[(&str, String)]) -> String {
    let mut fields = vec![("group", "fixture-a".into()), ("before", before.into())];
    fields.extend_from_slice(filters);
    query("/api/records", &fields)
}
async fn initial(relay: &Relay) -> Result<()> {
    relay.api.select(&["fixture-a"]).await?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    eventually(|| async {
        Ok((relay.rows("fixture-a").await?.as_array().unwrap().len() == 40).then_some(()))
    })
    .await
}
pub async fn records_reach_native_eof_without_duplicates_lost_tail_or_insert_order_sorting()
-> Result<()> {
    let mut relay = Relay::new("history").await?;
    initial(&relay).await?;
    let mut before = String::new();
    let mut found = BTreeSet::new();
    let mut ended = false;
    for _ in 0..10 {
        let value = relay.api.get(&path(&before, &[])).await?;
        assert_eq!(value["pending"], false);
        let rows = value["messages"].as_array().unwrap();
        assert!(rows.len() <= 50);
        assert!(
            rows.windows(2)
                .all(|w| (w[0]["timestamp"].as_i64(), w[0]["seq"].as_i64())
                    > (w[1]["timestamp"].as_i64(), w[1]["seq"].as_i64()))
        );
        for row in rows {
            assert!(
                found.insert(row["id"].as_str().unwrap().to_owned()),
                "duplicate record"
            );
        }
        if value["has_more"] == false {
            assert_eq!(value["next_before"], Value::Null);
            ended = true;
            break;
        }
        let next = value["next_before"].as_str().unwrap().to_owned();
        assert_ne!(next, before);
        before = next;
    }
    assert!(ended);
    assert_eq!(found.len(), 137);
    assert_eq!(relay.commands("history")?.len(), 3);
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(relay.api.get(&path(&before, &[])).await?["has_more"], false);
    assert_eq!(relay.commands("history")?.len(), 3);
    Ok(())
}
pub async fn filtered_empty_pages_keep_advancing_until_an_older_native_match() -> Result<()> {
    let relay = Relay::new("history").await?;
    initial(&relay).await?;
    let mut before = String::new();
    let mut found = Vec::new();
    let mut empty = 0;
    let mut ended = false;
    for _ in 0..10 {
        let value = relay
            .api
            .get(&path(&before, &[("q", "Offline row 1".into())]))
            .await?;
        let rows = value["messages"].as_array().unwrap();
        if rows.is_empty() {
            empty += 1;
        }
        found.extend(rows.iter().cloned());
        if value["has_more"] == false {
            ended = true;
            break;
        }
        let next = value["next_before"].as_str().unwrap().to_owned();
        assert_ne!(next, before);
        before = next;
    }
    assert!(ended);
    assert!(empty > 0);
    assert!(found.iter().any(|r| r["text"] == "Offline row 1"));
    let voice = relay
        .api
        .get(&path("", &[("type", "voice".into())]))
        .await?;
    assert_eq!(voice["messages"], json!([]));
    assert_eq!(voice["has_more"], false);
    assert_eq!(voice["next_before"], Value::Null);
    Ok(())
}
pub async fn announcements_are_readonly_authorized_and_distinguish_unset_from_unavailable()
-> Result<()> {
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    let key = relay.api.key(json!({"groups":["fixture-a"]})).await?;
    let token = key["token"].as_str().unwrap();
    relay
        .api
        .deny("/api/groups/fixture-b/announcement", None, Some(token), 403)
        .await?;
    relay
        .api
        .deny("/api/records?group=fixture-b", None, Some(token), 403)
        .await?;
    let endpoint = "/api/groups/fixture-a/announcement";
    let value = relay.api.get_as(endpoint, token).await?;
    assert_eq!(value["available"], true);
    assert_eq!(value["set"], true);
    assert_eq!(value["text"], "Offline announcement");
    assert_eq!(value["updated_at"], 1700000000123i64);
    assert!(value.get("path").is_none());
    assert!(value.get("request_id").is_none());
    std::fs::write(relay.dir.path().join("announcement-empty"), [])?;
    let value = relay.api.get_as(endpoint, token).await?;
    assert_eq!(value["available"], true);
    assert_eq!(value["set"], false);
    assert_eq!(value["text"], "");
    std::fs::write(relay.dir.path().join("announcement-unavailable"), [])?;
    let value = relay.api.get_as(endpoint, token).await?;
    assert_eq!(value["available"], false);
    assert_eq!(value["set"], false);
    assert_eq!(value["updated_at"], Value::Null);
    relay.api.update(&key, json!({"groups":[]})).await?;
    relay.api.deny(endpoint, None, Some(token), 403).await?;
    relay
        .api
        .deny("/api/records?group=fixture-a", None, Some(token), 403)
        .await
}
pub async fn announcement_timeout_deduplicates_waiters_and_ignores_the_old_generation() -> Result<()>
{
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a"]).await?;
    let hold = relay.dir.path().join("hold-announcement");
    std::fs::write(&hold, [])?;
    let endpoint = "/api/groups/fixture-a/announcement";
    let (a, b) = tokio::join!(relay.api.get(endpoint), relay.api.get(endpoint));
    for value in [a?, b?] {
        assert_eq!(value["available"], false);
        assert_eq!(value["pending"], true);
    }
    assert_eq!(relay.commands("announcement")?.len(), 1);
    std::fs::remove_file(hold)?;
    assert_eq!(relay.api.get(endpoint).await?["available"], true);
    let jobs = relay.commands("announcement")?;
    assert_eq!(jobs.len(), 2);
    assert_ne!(jobs[0]["request_id"], jobs[1]["request_id"]);
    Ok(())
}
