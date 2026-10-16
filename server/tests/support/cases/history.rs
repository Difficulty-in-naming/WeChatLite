use super::super::*;

fn path(group: &str, before: i64) -> String {
    format!("/api/history?group={group}&before={before}")
}
pub async fn pages_beyond_initial_40_are_ordered_deduplicated_and_end_only_after_native_eof()
-> Result<()> {
    let mut relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a"]).await?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    eventually(|| async {
        Ok((relay.rows("fixture-a").await?.as_array().unwrap().len() == 40).then_some(()))
    })
    .await?;
    let mut before = 0;
    let mut found = std::collections::BTreeMap::new();
    let mut ended = false;
    for _ in 0..8 {
        let value = relay.api.get(&path("fixture-a", before)).await?;
        assert_eq!(value["pending"], false);
        let rows = value["messages"].as_array().unwrap();
        assert!(
            rows.windows(2)
                .all(|w| (w[0]["timestamp"].as_i64(), w[0]["seq"].as_i64())
                    < (w[1]["timestamp"].as_i64(), w[1]["seq"].as_i64()))
        );
        for row in rows {
            found.insert(row["id"].as_str().unwrap().to_owned(), row.clone());
        }
        if let Some(oldest) = found
            .values()
            .min_by_key(|r| (r["timestamp"].as_i64(), r["seq"].as_i64()))
        {
            before = oldest["seq"].as_i64().unwrap();
        }
        if value["has_more"] == false {
            ended = true;
            break;
        }
    }
    assert!(ended);
    assert_eq!(found.len(), 137);
    assert_eq!(relay.commands("history")?.len(), 3);
    let end = relay.api.get(&path("fixture-a", before)).await?;
    assert_eq!(end["has_more"], false);
    assert!(
        end["messages"]
            .as_array()
            .unwrap()
            .iter()
            .all(|r| found.contains_key(r["id"].as_str().unwrap()))
    );
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(
        relay.api.get(&path("fixture-a", before)).await?["has_more"],
        false
    );
    assert_eq!(relay.commands("history")?.len(), 3);
    Ok(())
}
pub async fn timeout_is_pending_and_same_cursor_requests_share_one_native_task() -> Result<()> {
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a"]).await?;
    let hold = relay.dir.path().join("hold-history");
    std::fs::write(&hold, [])?;
    let endpoint = path("fixture-a", 0);
    let (a, b) = tokio::join!(relay.api.get(&endpoint), relay.api.get(&endpoint));
    for value in [a?, b?] {
        assert_eq!(value, json!({"messages":[],"has_more":true,"pending":true}));
    }
    assert_eq!(relay.commands("history")?.len(), 1);
    std::fs::remove_file(hold)?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    eventually(|| async {
        Ok((relay.rows("fixture-a").await?.as_array().unwrap().len() == 60).then_some(()))
    })
    .await?;
    assert_eq!(
        relay.api.get(&path("fixture-a", 0)).await?["pending"],
        false
    );
    assert_eq!(relay.commands("history")?.len(), 1);
    Ok(())
}
pub async fn group_and_before_validation_read_only_client_and_mid_request_revocation() -> Result<()>
{
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    let key = relay.api.key(json!({"groups":["fixture-a"]})).await?;
    let token = key["token"].as_str().unwrap();
    for (path, code) in [
        ("/api/history?group=fixture-b", 403),
        ("/api/history?group=fixture-a&before=-1", 400),
        ("/api/history?group=fixture-a&before=999999", 400),
        ("/api/history?group=fixture-a&search=text", 400),
    ] {
        relay.api.deny(path, None, Some(token), code).await?;
    }
    let hold = relay.dir.path().join("hold-history");
    std::fs::write(&hold, [])?;
    let api = relay.api.clone();
    let token_owned = token.to_owned();
    let pending = tokio::spawn(async move {
        api.raw(
            Method::GET,
            &path("fixture-a", 0),
            None,
            None,
            Some(&token_owned),
        )
        .await
    });
    eventually(|| async { Ok((!relay.commands("history")?.is_empty()).then_some(())) }).await?;
    relay.api.update(&key, json!({"groups":[]})).await?;
    std::fs::remove_file(hold)?;
    relay
        .api
        .post("/api/groups/fixture-b/activate", json!({}))
        .await?;
    assert_eq!(pending.await??.status(), 403);
    relay
        .api
        .update(&key, json!({"groups":["fixture-a"]}))
        .await?;
    assert!(
        !relay.api.get_as(&path("fixture-a", 0), token).await?["messages"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    let foreign = relay.nonempty("fixture-b").await?[0]["seq"]
        .as_i64()
        .unwrap();
    relay
        .api
        .deny(&path("fixture-a", foreign), None, Some(token), 400)
        .await
}
pub async fn native_error_is_retryable_failure_and_does_not_persist_false_eof() -> Result<()> {
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a"]).await?;
    let error = relay.dir.path().join("history-error");
    std::fs::write(&error, [])?;
    relay
        .api
        .deny(&path("fixture-a", 0), None, None, 503)
        .await?;
    std::fs::remove_file(error)?;
    assert!(
        !relay.api.get(&path("fixture-a", 0)).await?["messages"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(relay.commands("history")?.len(), 2);
    Ok(())
}
pub async fn history_events_remain_marked_in_live_broadcast_and_durable_replay() -> Result<()> {
    let relay = Relay::new("history").await?;
    relay.api.select(&["fixture-a"]).await?;
    let mut socket = relay.api.ws(-1, None).await?;
    sync(&mut socket).await?;
    assert_eq!(
        relay.api.get(&path("fixture-a", 0)).await?["messages"]
            .as_array()
            .unwrap()
            .len(),
        60
    );
    let mut count = 0;
    while count < 60 {
        let event = recv(&mut socket).await?;
        if event["type"] == "message" {
            assert_eq!(event["historical"], true);
            count += 1;
        }
    }
    let mut socket = relay.api.ws(0, None).await?;
    let events = sync(&mut socket).await?;
    let messages = events
        .iter()
        .filter(|e| e["type"] == "message")
        .collect::<Vec<_>>();
    assert_eq!(messages.len(), 60);
    assert!(messages.iter().all(|e| e["historical"] == true));
    Ok(())
}
