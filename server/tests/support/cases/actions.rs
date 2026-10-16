use super::super::*;

pub async fn locate_requires_current_group_selection_and_never_returns_recalled_content()
-> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    let original = relay.confirmed("Locate original", json!({})).await?;
    let id = original["id"].as_str().unwrap();
    let path = format!("/api/messages/{id}");
    let located = relay.api.get(&format!("{path}?group=fixture-a")).await?;
    assert_eq!(located["id"], id);
    assert!(located["seq"].as_i64().unwrap() > 0);
    assert_eq!(located["text"], "Locate original");
    relay
        .api
        .deny(&format!("{path}?group=fixture-b"), None, None, 404)
        .await?;
    relay.api.select(&["fixture-b"]).await?;
    relay
        .api
        .deny(&format!("{path}?group=fixture-a"), None, None, 403)
        .await?;
    relay.api.select(&["fixture-a"]).await?;
    relay.api.post(&format!("{path}/recall"), json!({})).await?;
    relay.state(id, "recalled").await?;
    relay
        .api
        .deny(&format!("{path}?group=fixture-a"), None, None, 410)
        .await
}
pub async fn quote_canonical_snapshot_cross_group_forgery_and_withdrawal() -> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    let original = relay.confirmed("Original quote body", json!({})).await?;
    let id = original["id"].as_str().unwrap();
    for quote in [
        json!({"id":""}),
        json!({"id":"missing"}),
        json!({"id":id,"sender":"Forged name"}),
        json!({"id":id,"text":"Forged text"}),
        json!({"id":id,"unavailable":true}),
    ] {
        let mut value = message("fixture-a", "Reply");
        value["quote"] = quote;
        relay
            .api
            .deny("/api/messages", Some(value), None, 400)
            .await?;
    }
    let mut foreign = message("fixture-b", "Cross group");
    foreign["quote"] = json!({"id":id});
    relay
        .api
        .deny("/api/messages", Some(foreign), None, 400)
        .await?;
    let reply = relay.confirmed("Reply", json!({"quote":{"id":id}})).await?;
    assert_eq!(
        reply["quote"],
        json!({"id":id,"sender":original["sender"],"text":original["text"],"unavailable":false})
    );
    assert_eq!(original["source_ids"].as_array().unwrap().len(), 2);
    let endpoint = format!("/api/messages/{id}/recall");
    assert_eq!(
        relay.api.post(&endpoint, json!({})).await?["recall_status"],
        "pending"
    );
    assert_eq!(relay.state(id, "recalled").await?["text"], "");
    let quote = relay.stored(reply["id"].as_str().unwrap())?.unwrap()["quote"].clone();
    assert_eq!(quote["unavailable"], true);
    assert_eq!(quote["text"], "");
    let mut withdrawn = message("fixture-a", "Cannot reference recalled");
    withdrawn["quote"] = json!({"id":id});
    relay
        .api
        .deny("/api/messages", Some(withdrawn), None, 400)
        .await?;
    let mut socket = relay.api.ws(0, None).await?;
    assert!(!serde_json::to_string(&sync(&mut socket).await?)?.contains("Original quote body"));
    assert_eq!(
        relay.api.post(&endpoint, json!({})).await?["status"],
        "recalled"
    );
    Ok(())
}
pub async fn recall_requires_token_selection_owner_confirmation_and_time_window() -> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    let incoming = relay.nonempty("fixture-a").await?[0].clone();
    relay
        .api
        .deny(
            &format!("/api/messages/{}/recall", incoming["id"].as_str().unwrap()),
            Some(json!({})),
            None,
            400,
        )
        .await?;
    for timestamp in [now() - 181000, now() + 60000] {
        let original = relay.confirmed("Fixture original", json!({})).await?;
        let id = original["id"].as_str().unwrap();
        relay.change(id, json!({"timestamp":timestamp}))?;
        relay
            .api
            .deny(
                &format!("/api/messages/{id}/recall"),
                Some(json!({})),
                None,
                400,
            )
            .await?;
    }
    let unconfirmed = relay.confirmed("Fixture unconfirmed", json!({})).await?;
    let id = unconfirmed["id"].as_str().unwrap();
    relay.change(id, json!({"source_id":"","source_ids":[]}))?;
    relay
        .api
        .deny(
            &format!("/api/messages/{id}/recall"),
            Some(json!({})),
            None,
            400,
        )
        .await?;
    let original = relay.confirmed("Fixture original", json!({})).await?;
    let endpoint = format!("/api/messages/{}/recall", original["id"].as_str().unwrap());
    relay
        .api
        .deny(&endpoint, Some(json!({})), Some("invalid"), 401)
        .await?;
    relay.api.select(&[]).await?;
    relay.api.deny(&endpoint, Some(json!({})), None, 403).await
}
pub async fn native_quote_and_reply_have_independent_utf8_capacity_before_enqueue() -> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let original = relay.confirmed(&"字".repeat(7900), json!({})).await?;
    let mut oversized = message("fixture-a", &"x".repeat(24001));
    oversized["quote"] = json!({"id":original["id"]});
    relay
        .api
        .deny("/api/messages", Some(oversized.clone()), None, 400)
        .await?;
    assert!(relay.stored(oversized["id"].as_str().unwrap())?.is_none());
    let accepted = relay
        .confirmed(&"x".repeat(24000), json!({"quote":{"id":original["id"]}}))
        .await?;
    assert_eq!(accepted["text"].as_str().unwrap().len(), 24000);
    assert_eq!(accepted["quote"]["text"], original["text"]);
    Ok(())
}
pub async fn pending_recall_survives_restart_as_check_and_never_reissues_action() -> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let original = relay
        .confirmed("recall hold until restart", json!({}))
        .await?;
    let id = original["id"].as_str().unwrap();
    let endpoint = format!("/api/messages/{id}/recall");
    relay.api.post(&endpoint, json!({})).await?;
    relay.api.post(&endpoint, json!({})).await?;
    eventually(|| async { Ok((!relay.commands("recall")?.is_empty()).then_some(())) }).await?;
    relay.stop().await?;
    assert_eq!(relay.stored(id)?.unwrap()["recall_status"], "pending");
    relay.start().await?;
    relay.state(id, "recalled").await?;
    assert_eq!(relay.commands("recall")?.len(), 1);
    assert_eq!(relay.commands("recall_check")?.len(), 1);
    Ok(())
}
pub async fn failed_or_uncertain_confirmation_preserves_body_and_does_not_retry() -> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    for status in ["failed", "uncertain"] {
        let original = relay
            .confirmed(
                if status == "failed" {
                    "recall fail"
                } else {
                    "recall uncertain"
                },
                json!({}),
            )
            .await?;
        let id = original["id"].as_str().unwrap();
        let endpoint = format!("/api/messages/{id}/recall");
        relay.api.post(&endpoint, json!({})).await?;
        eventually(|| async { Ok(relay.stored(id)?.filter(|v| v["recall_status"] == status)) })
            .await?;
        assert_eq!(relay.stored(id)?.unwrap()["text"], original["text"]);
        assert_eq!(
            relay.api.post(&endpoint, json!({})).await?["recall_status"],
            status
        );
    }
    assert_eq!(relay.commands("recall")?.len(), 2);
    Ok(())
}
