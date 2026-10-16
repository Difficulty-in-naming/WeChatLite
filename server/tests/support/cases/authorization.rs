use super::super::*;
use sha2::{Digest, Sha256};

pub async fn default_empty_local_management_validation_and_restart() -> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    assert_eq!(relay.api.get("/api/groups").await?, json!([]));
    let snapshot = relay.api.get("/api/local/forwarding").await?;
    assert_eq!(snapshot["selected"], json!([]));
    assert_eq!(
        ids(&snapshot["groups"]),
        BTreeSet::from(["fixture-a".into(), "fixture-b".into()])
    );
    for body in [None, Some(json!({"selected":["fixture-a"]}))] {
        relay
            .api
            .deny("/api/local/forwarding", body, Some("invalid"), 401)
            .await?;
    }
    relay
        .api
        .deny(
            "/api/local/forwarding",
            Some(json!({"selected":["unknown"]})),
            None,
            400,
        )
        .await?;
    assert_eq!(
        relay.api.get("/api/local/forwarding").await?["selected"],
        json!([])
    );
    assert_eq!(
        relay.api.select(&["fixture-a", "fixture-a"]).await?["selected"],
        json!(["fixture-a"])
    );
    assert_eq!(
        ids(&relay.api.get("/api/groups").await?),
        BTreeSet::from(["fixture-a".into()])
    );
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(
        serde_json::from_slice::<Value>(&std::fs::read(
            relay.dir.path().join("initial-policy.json")
        )?)?,
        json!(["fixture-a"])
    );
    assert_eq!(
        relay.api.get("/api/local/forwarding").await?["selected"],
        json!(["fixture-a"])
    );
    relay.api.select(&[]).await?;
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(relay.api.get("/api/groups").await?, json!([]));
    assert_eq!(
        serde_json::from_slice::<Value>(&std::fs::read(
            relay.dir.path().join("initial-policy.json")
        )?)?,
        json!([])
    );
    Ok(())
}

async fn imported(relay: &Relay) -> Result<(Value, Value)> {
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    Ok((
        relay.nonempty("fixture-a").await?[0].clone(),
        relay.nonempty("fixture-b").await?[0].clone(),
    ))
}
pub async fn history_members_media_old_cursors_and_live_events_cannot_bypass_selection()
-> Result<()> {
    let relay = Relay::new("forwarding").await?;
    let (a, b) = imported(&relay).await?;
    let uploaded = relay
        .api
        .upload("fixture-a", "owned.txt", b"owned-by-a".to_vec(), None)
        .await?;
    let missing = relay
        .api
        .raw(
            Method::POST,
            "/api/media?name=missing-owner.txt",
            None,
            Some(b"data".to_vec()),
            None,
        )
        .await?;
    assert_eq!(missing.status(), 400);
    let mut invalid = message("fixture-b", "cross-group media");
    invalid["media"] = json!([uploaded["id"]]);
    relay
        .api
        .deny("/api/messages", Some(invalid), None, 400)
        .await?;
    let outgoing = message("fixture-b", "before deselection");
    relay.api.post("/api/messages", outgoing.clone()).await?;
    relay
        .state(outgoing["id"].as_str().unwrap(), "sent")
        .await?;
    relay.api.select(&["fixture-a"]).await?;
    for (path, body) in [
        ("/api/messages?group=fixture-b".into(), None),
        ("/api/groups/fixture-b/members".into(), None),
        ("/api/groups/fixture-b/activate".into(), Some(json!({}))),
        ("/api/messages".into(), Some(outgoing)),
        (
            format!("/api/media/{}", b["media"][0]["id"].as_str().unwrap()),
            None,
        ),
    ] {
        relay.api.deny(&path, body, None, 403).await?;
    }
    let denied = relay
        .api
        .raw(
            Method::POST,
            "/api/media?group=fixture-b&name=denied.txt",
            None,
            Some(b"data".to_vec()),
            None,
        )
        .await?;
    assert_eq!(denied.status(), 403);
    let mut socket = relay.api.ws(0, None).await?;
    let events = sync(&mut socket).await?;
    assert!(
        events
            .iter()
            .filter(|e| e["type"] == "message")
            .all(|e| e["message"]["group"] == "fixture-a")
    );
    assert!(!serde_json::to_string(&events)?.contains("Private body fixture-b"));
    assert!(!serde_json::to_string(&events)?.contains(b["media"][0]["id"].as_str().unwrap()));
    let mut live = relay.api.ws(-1, None).await?;
    sync(&mut live).await?;
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    eventually(|| async {
        let rows = relay.rows("fixture-a").await?;
        Ok((rows.as_array().unwrap().len() >= 2).then_some(()))
    })
    .await?;
    let barrier = message("fixture-a", "event barrier");
    relay.api.post("/api/messages", barrier.clone()).await?;
    loop {
        let event = recv(&mut live).await?;
        if event["type"] == "message" {
            assert_eq!(event["message"]["group"], "fixture-a");
            if event["message"]["id"] == barrier["id"] && event["message"]["status"] == "sent" {
                break;
            }
        }
    }
    assert!(relay.stored("fixture-b-2")?.is_none());
    {
        let connection = Connection::open_with_flags(
            relay.dir.path().join("state/relay.sqlite"),
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
        )?;
        assert_eq!(
            connection.query_row(
                "SELECT count(*) FROM media_owners WHERE group_id='fixture-b'",
                [],
                |r| r.get::<_, i64>(0)
            )?,
            1
        );
    }
    relay.api.select(&["fixture-b"]).await?;
    for id in [
        a["media"][0]["id"].as_str().unwrap(),
        uploaded["id"].as_str().unwrap(),
    ] {
        relay
            .api
            .deny(&format!("/api/media/{id}"), None, None, 403)
            .await?;
    }
    assert!(ids(&relay.rows("fixture-b").await?).contains(b["id"].as_str().unwrap()));
    Ok(())
}
pub async fn deselection_cancels_queued_send_and_confirms_submitted_send_without_retry()
-> Result<()> {
    let relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let first = message("fixture-a", "hold until revoked");
    let second = message("fixture-a", "must never submit");
    relay.api.post("/api/messages", first.clone()).await?;
    relay
        .state(first["id"].as_str().unwrap(), "sending")
        .await?;
    relay.api.post("/api/messages", second.clone()).await?;
    assert_eq!(
        relay.stored(second["id"].as_str().unwrap())?.unwrap()["status"],
        "queued"
    );
    relay.api.select(&[]).await?;
    relay.state(first["id"].as_str().unwrap(), "sent").await?;
    assert_eq!(
        relay.stored(second["id"].as_str().unwrap())?.unwrap()["status"],
        "failed"
    );
    relay
        .api
        .deny("/api/messages", Some(first.clone()), None, 403)
        .await?;
    relay.api.select(&["fixture-a"]).await?;
    assert_eq!(
        relay.api.post("/api/messages", first.clone()).await?["status"],
        "sent"
    );
    assert_eq!(
        relay
            .commands("send")?
            .iter()
            .map(|c| c["message"]["id"].clone())
            .collect::<Vec<_>>(),
        [first["id"].clone()]
    );
    Ok(())
}
pub async fn recoverable_credentials_defaults_and_restart_preserve_global_policy() -> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let key = relay.api.key(json!({})).await?;
    let token = key["token"].as_str().unwrap();
    let client = &key["client"];
    let credential_path = format!(
        "/api/local/clients/{}/credential",
        client["id"].as_str().unwrap()
    );
    for _ in 0..2 {
        let response = relay
            .api
            .raw(Method::GET, &credential_path, None, None, None)
            .await?;
        assert_eq!(response.status(), 200);
        assert_eq!(response.headers()["cache-control"], "no-store");
        assert_eq!(response.json::<Value>().await?, key);
    }
    assert_eq!(client["groups"], json!([]));
    assert_eq!(client["can_send"], false);
    assert_eq!(client["can_download"], false);
    assert_eq!(client["enabled"], true);
    assert_eq!(relay.api.get_as("/api/groups", token).await?, json!([]));
    let status = relay.api.get_as("/api/status", token).await?;
    assert_eq!(status["authenticated_client"]["id"], client["id"]);
    assert_eq!(status["authenticated_client"]["admin"], false);
    assert_eq!(
        status["permissions"],
        json!({"groups":[],"can_send":false,"can_download":false})
    );
    let listing = relay.api.get("/api/local/clients").await?;
    assert_eq!(listing["clients"], json!([client]));
    assert!(!listing.to_string().contains(token));
    assert!(!listing.to_string().contains("token_hash"));
    {
        let connection = Connection::open_with_flags(
            relay.dir.path().join("state/relay.sqlite"),
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
        )?;
        let (hash, body): (String, String) = connection.query_row(
            "SELECT token_hash,body FROM access_clients WHERE id=?1",
            [client["id"].as_str().unwrap()],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )?;
        assert_eq!(hash, format!("{:x}", Sha256::digest(token.as_bytes())));
        assert!(!body.contains(token));
        let secret: String = connection.query_row(
            "SELECT token FROM access_client_secrets WHERE client_id=?1",
            [client["id"].as_str().unwrap()],
            |row| row.get(0),
        )?;
        assert_eq!(secret, token);
    }
    relay
        .api
        .deny(
            "/api/local/clients",
            Some(json!({"name":"Bad group","groups":["missing"]})),
            None,
            400,
        )
        .await?;
    assert_eq!(
        relay.api.get("/api/local/clients").await?["clients"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    relay
        .api
        .update(
            &key,
            json!({"name":"Renamed","groups":["fixture-a","fixture-b"],"can_download":true}),
        )
        .await?;
    assert_eq!(
        ids(&relay.api.get_as("/api/groups", token).await?),
        BTreeSet::from(["fixture-a".into()])
    );
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(
        relay.api.get("/api/local/forwarding").await?["selected"],
        json!(["fixture-a"])
    );
    let clients = relay.api.get("/api/local/clients").await?;
    let retrieved = relay.api.get(&credential_path).await?;
    assert_eq!(retrieved["token"], token);
    assert_eq!(retrieved["client"], clients["clients"][0]);
    assert_eq!(clients["clients"][0]["name"], "Renamed");
    assert_eq!(
        clients["clients"][0]["groups"],
        json!(["fixture-a", "fixture-b"])
    );
    assert_eq!(
        ids(&relay.api.get_as("/api/groups", token).await?),
        BTreeSet::from(["fixture-a".into()])
    );
    Ok(())
}

pub async fn credential_rotation_preserves_policy_invalidates_old_sessions_and_revocation_erases_secret()
-> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let key = relay
        .api
        .key(json!({"name":"Rotate fixture", "groups":["fixture-a","fixture-b"], "can_send":true}))
        .await?;
    let other = relay
        .api
        .key(json!({"name":"Unaffected", "groups":["fixture-a"]}))
        .await?;
    let id = key["client"]["id"].as_str().unwrap();
    let old_token = key["token"].as_str().unwrap();
    let root = format!("/api/local/clients/{id}");
    let mut socket = relay.api.ws(-1, Some(old_token)).await?;
    let mut unaffected = relay.api.ws(-1, other["token"].as_str()).await?;
    sync(&mut socket).await?;
    sync(&mut unaffected).await?;
    let rotated = relay.api.post(&format!("{root}/rotate"), json!({})).await?;
    let token = rotated["token"].as_str().unwrap();
    assert_ne!(token, old_token);
    for field in [
        "id",
        "name",
        "groups",
        "enabled",
        "can_send",
        "can_download",
        "created_at",
    ] {
        assert_eq!(rotated["client"][field], key["client"][field], "{field}");
    }
    assert_eq!(
        rotated["client"]["revision"].as_u64().unwrap(),
        key["client"]["revision"].as_u64().unwrap() + 1
    );
    closed(&mut socket).await?;
    relay
        .api
        .deny("/api/status", None, Some(old_token), 401)
        .await?;
    assert!(relay.api.ws(-1, Some(old_token)).await.is_err());
    assert_eq!(
        relay.api.get(&format!("{root}/credential")).await?["token"],
        token
    );
    assert_eq!(
        relay.api.get_as("/api/status", token).await?["permissions"],
        json!({"groups":["fixture-a"],"can_send":true,"can_download":false})
    );
    relay
        .api
        .post_as(
            "/api/groups/fixture-a/activate",
            json!({}),
            other["token"].as_str().unwrap(),
        )
        .await?;
    loop {
        if recv(&mut unaffected).await?["type"] == "message" {
            break;
        }
    }
    unaffected.close(None).await?;
    for value in [
        relay.api.get("/api/local/clients").await?,
        relay.api.get_as("/api/status", token).await?,
    ] {
        let text = value.to_string();
        assert!(!text.contains(token) && !text.contains(old_token) && !text.contains("token_hash"));
    }
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(
        relay.api.get(&format!("{root}/credential")).await?["token"],
        token
    );
    relay.api.update(&rotated, json!({"enabled":false})).await?;
    let disabled_rotation = relay.api.post(&format!("{root}/rotate"), json!({})).await?;
    assert_eq!(disabled_rotation["client"]["enabled"], false);
    let latest_token = disabled_rotation["token"].as_str().unwrap();
    assert_ne!(latest_token, token);
    relay
        .api
        .deny("/api/status", None, Some(token), 401)
        .await?;
    relay
        .api
        .deny("/api/status", None, Some(latest_token), 401)
        .await?;
    relay
        .api
        .update(&disabled_rotation, json!({"enabled":true}))
        .await?;
    let mut socket = relay.api.ws(-1, Some(latest_token)).await?;
    sync(&mut socket).await?;
    relay.api.post(&format!("{root}/revoke"), json!({})).await?;
    closed(&mut socket).await?;
    relay
        .api
        .deny(&format!("{root}/credential"), None, None, 403)
        .await?;
    relay
        .api
        .deny(&format!("{root}/rotate"), Some(json!({})), None, 403)
        .await?;
    relay
        .api
        .deny("/api/status", None, Some(latest_token), 401)
        .await?;
    let connection = Connection::open_with_flags(
        relay.dir.path().join("state/relay.sqlite"),
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    )?;
    assert_eq!(
        connection.query_row(
            "SELECT count(*) FROM access_client_secrets WHERE client_id=?1",
            [id],
            |row| row.get::<_, i64>(0)
        )?,
        0
    );
    drop(connection);
    relay.stop().await?;
    relay.start().await?;
    relay
        .api
        .deny(&format!("{root}/credential"), None, None, 403)
        .await?;
    relay
        .api
        .deny(&format!("{root}/rotate"), Some(json!({})), None, 403)
        .await?;
    Ok(())
}

pub async fn legacy_hash_only_key_learns_original_token_after_successful_authentication_and_restart()
-> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    relay.stop().await?;
    let token = "fixture-installed-legacy-client-secret";
    let hash = format!("{:x}", Sha256::digest(token.as_bytes()));
    let access = json!({"id":"legacy-fixture", "name":"Legacy fixture", "enabled":true, "revoked":false,
        "groups":["fixture-a"],"can_send":false,"can_download":false,"created_at":1,"updated_at":1,"revision":7});
    let path = relay.dir.path().join("state/relay.sqlite");
    {
        let connection = Connection::open(&path)?;
        connection.execute(
            "INSERT INTO access_clients(id,token_hash,body) VALUES(?1,?2,?3)",
            params!["legacy-fixture", hash, access.to_string()],
        )?;
    }
    relay.start().await?;
    let endpoint = "/api/local/clients/legacy-fixture/credential";
    let before = relay.api.get(endpoint).await?;
    assert!(before["token"].is_null());
    assert_eq!(before["client"], access);
    relay
        .api
        .deny("/api/status", None, Some("rejected-legacy-token"), 401)
        .await?;
    assert!(relay.api.get(endpoint).await?["token"].is_null());
    assert_eq!(
        relay.api.get_as("/api/status", token).await?["authenticated_client"]["id"],
        "legacy-fixture"
    );
    let learned = relay.api.get(endpoint).await?;
    assert_eq!(learned["token"], token);
    assert_eq!(learned["client"], access);
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(relay.api.get(endpoint).await?, learned);
    assert_eq!(relay.api.get(endpoint).await?, learned);
    assert!(
        !relay
            .api
            .get("/api/local/clients")
            .await?
            .to_string()
            .contains(token)
    );
    let connection =
        Connection::open_with_flags(&path, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY)?;
    assert_eq!(
        connection.query_row(
            "SELECT token FROM access_client_secrets WHERE client_id='legacy-fixture'",
            [],
            |row| row.get::<_, String>(0)
        )?,
        token
    );
    Ok(())
}
pub async fn client_key_never_gains_local_administrator_and_spoofed_headers_do_not_help()
-> Result<()> {
    let relay = Relay::new("forwarding").await?;
    let key = relay
        .api
        .key(json!({"groups":["fixture-a"],"can_send":true,"can_download":true}))
        .await?;
    let token = key["token"].as_str().unwrap();
    let id = key["client"]["id"].as_str().unwrap();
    for (path, body) in [
        ("/api/local/forwarding".into(), None),
        (
            "/api/local/forwarding".into(),
            Some(json!({"selected":["fixture-a"]})),
        ),
        ("/api/local/clients".into(), None),
        (
            "/api/local/clients".into(),
            Some(json!({"name":"Escalation"})),
        ),
        (
            format!("/api/local/clients/{id}"),
            Some(json!({"can_send":true})),
        ),
        (format!("/api/local/clients/{id}/revoke"), Some(json!({}))),
        (format!("/api/local/clients/{id}/credential"), None),
        (format!("/api/local/clients/{id}/rotate"), Some(json!({}))),
        ("/api/shutdown".into(), Some(json!({}))),
    ] {
        relay.api.deny(&path, body, Some(token), 403).await?;
    }
    let response = relay
        .api
        .client
        .get(format!("{}/api/local/clients", relay.api.base))
        .bearer_auth(token)
        .header("X-Forwarded-For", "127.0.0.1")
        .header("X-Admin", "true")
        .send()
        .await?;
    assert_eq!(response.status(), 403);
    assert_eq!(relay.api.get("/api/status").await?["ready"], true);
    Ok(())
}
pub async fn group_send_download_quote_history_and_replay_boundaries() -> Result<()> {
    let relay = Relay::new("forwarding").await?;
    let (a, b) = imported(&relay).await?;
    let key = relay.api.key(json!({"groups":["fixture-a"]})).await?;
    let token = key["token"].as_str().unwrap();
    assert_eq!(
        ids(&relay.api.get_as("/api/groups", token).await?),
        BTreeSet::from(["fixture-a".into()])
    );
    assert_eq!(
        relay
            .api
            .get_as("/api/messages?group=fixture-a", token)
            .await?[0]["id"],
        a["id"]
    );
    assert!(
        !relay
            .api
            .get_as("/api/groups/fixture-a/members", token)
            .await?
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        relay
            .api
            .get_as(
                &format!(
                    "/api/messages/{}?group=fixture-a",
                    a["id"].as_str().unwrap()
                ),
                token
            )
            .await?["id"],
        a["id"]
    );
    for (path, body) in [
        ("/api/messages?group=fixture-b".into(), None),
        ("/api/groups/fixture-b/members".into(), None),
        ("/api/groups/fixture-b/activate".into(), Some(json!({}))),
        (
            format!(
                "/api/messages/{}?group=fixture-b",
                b["id"].as_str().unwrap()
            ),
            None,
        ),
        (
            "/api/messages".into(),
            Some(message("fixture-b", "blocked")),
        ),
        (
            format!("/api/media/{}", b["media"][0]["id"].as_str().unwrap()),
            None,
        ),
        (
            format!("/api/media/{}", a["media"][0]["id"].as_str().unwrap()),
            None,
        ),
    ] {
        relay.api.deny(&path, body, Some(token), 403).await?;
    }
    let response = relay
        .api
        .raw(
            Method::POST,
            "/api/media?group=fixture-a&name=blocked.txt",
            None,
            Some(b"blocked".to_vec()),
            Some(token),
        )
        .await?;
    assert_eq!(response.status(), 403);
    let mut spoof = message("fixture-a", "blocked");
    spoof["client_id"] = json!("local-admin");
    relay
        .api
        .deny("/api/messages", Some(spoof), Some(token), 403)
        .await?;
    let original = relay
        .confirmed("Shared outgoing original", json!({}))
        .await?;
    relay
        .api
        .deny(
            &format!("/api/messages/{}/recall", original["id"].as_str().unwrap()),
            Some(json!({})),
            Some(token),
            403,
        )
        .await?;
    let mut socket = relay.api.ws(0, Some(token)).await?;
    let events = sync(&mut socket).await?;
    let serialized = serde_json::to_string(&events)?;
    assert!(!serialized.contains("fixture-b"));
    assert!(!serialized.contains("access_client"));
    relay
        .api
        .update(&key, json!({"can_send":true,"can_download":true}))
        .await?;
    assert_eq!(
        relay
            .api
            .download(a["media"][0]["id"].as_str().unwrap(), Some(token))
            .await?,
        b"private bytes fixture-a"
    );
    relay
        .api
        .deny(
            &format!("/api/media/{}", b["media"][0]["id"].as_str().unwrap()),
            None,
            Some(token),
            403,
        )
        .await?;
    let mut forged = message("fixture-a", "bad quote");
    forged["quote"] = json!({"id":b["id"]});
    relay
        .api
        .deny("/api/messages", Some(forged), Some(token), 400)
        .await?;
    let upload = relay
        .api
        .upload(
            "fixture-a",
            "allowed.txt",
            b"client attachment".to_vec(),
            Some(token),
        )
        .await?;
    let mut outgoing = message("fixture-a", "Independent send");
    outgoing["media"] = json!([upload["id"]]);
    let submitted = relay
        .api
        .post_as("/api/messages", outgoing.clone(), token)
        .await?;
    assert!(submitted.get("access_client").is_none());
    let confirmed = relay
        .state(outgoing["id"].as_str().unwrap(), "sent")
        .await?;
    assert_eq!(confirmed["access_client"], key["client"]["id"]);
    relay
        .api
        .post_as(
            &format!("/api/messages/{}/recall", outgoing["id"].as_str().unwrap()),
            json!({}),
            token,
        )
        .await?;
    relay
        .state(outgoing["id"].as_str().unwrap(), "recalled")
        .await?;
    relay.api.select(&["fixture-b"]).await?;
    assert_eq!(relay.api.get_as("/api/groups", token).await?, json!([]));
    relay
        .api
        .deny("/api/messages?group=fixture-a", None, Some(token), 403)
        .await?;
    relay
        .api
        .deny(
            &format!("/api/media/{}", upload["id"].as_str().unwrap()),
            None,
            Some(token),
            403,
        )
        .await?;
    Ok(())
}
pub async fn permission_update_closes_only_changed_session_and_reconnect_filters_old_and_live_events()
-> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a", "fixture-b"]).await?;
    let first = relay
        .api
        .key(json!({"name":"First","groups":["fixture-a","fixture-b"]}))
        .await?;
    let other = relay
        .api
        .key(json!({"name":"Other","groups":["fixture-a"]}))
        .await?;
    let token = first["token"].as_str().unwrap();
    let other_token = other["token"].as_str().unwrap();
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    relay.nonempty("fixture-b").await?;
    let mut socket = relay.api.ws(-1, Some(token)).await?;
    let mut unaffected = relay.api.ws(-1, Some(other_token)).await?;
    sync(&mut socket).await?;
    sync(&mut unaffected).await?;
    assert_eq!(relay.api.get("/api/status").await?["online_clients"], 2);
    let mut duplicate = relay.api.ws(-1, Some(token)).await?;
    sync(&mut duplicate).await?;
    assert_eq!(relay.api.get("/api/status").await?["online_clients"], 2);
    duplicate.close(None).await?;
    relay
        .api
        .update(&first, json!({"groups":["fixture-a"]}))
        .await?;
    closed(&mut socket).await?;
    eventually(|| async {
        Ok((relay.api.get("/api/status").await?["online_clients"] == 1).then_some(()))
    })
    .await?;
    relay
        .api
        .post_as("/api/groups/fixture-a/activate", json!({}), other_token)
        .await?;
    loop {
        let event = recv(&mut unaffected).await?;
        if event["type"] == "message" {
            assert_eq!(event["message"]["group"], "fixture-a");
            break;
        }
    }
    unaffected.close(None).await?;
    let mut socket = relay.api.ws(0, Some(token)).await?;
    let events = sync(&mut socket).await?;
    assert!(!serde_json::to_string(&events)?.contains("fixture-b"));
    relay
        .api
        .post("/api/groups/fixture-a/activate", json!({}))
        .await?;
    loop {
        let event = recv(&mut socket).await?;
        if event["type"] == "message" {
            assert_eq!(event["message"]["group"], "fixture-a");
            break;
        }
    }
    relay.api.update(&first, json!({"enabled":false})).await?;
    closed(&mut socket).await?;
    relay
        .api
        .deny("/api/groups", None, Some(token), 401)
        .await?;
    let error = relay.api.ws(0, Some(token)).await.unwrap_err();
    match error.downcast_ref::<tokio_tungstenite::tungstenite::Error>() {
        Some(tokio_tungstenite::tungstenite::Error::Http(response)) => {
            assert_eq!(response.status().as_u16(), 401)
        }
        _ => bail!("disabled WS did not return HTTP 401"),
    }
    relay.api.update(&first, json!({"enabled":true})).await?;
    assert!(
        !relay
            .api
            .get_as("/api/groups", token)
            .await?
            .as_array()
            .unwrap()
            .is_empty()
    );
    relay
        .api
        .post(
            &format!(
                "/api/local/clients/{}/revoke",
                first["client"]["id"].as_str().unwrap()
            ),
            json!({}),
        )
        .await?;
    relay
        .api
        .deny("/api/status", None, Some(token), 401)
        .await?;
    relay
        .api
        .deny(
            &format!(
                "/api/local/clients/{}",
                first["client"]["id"].as_str().unwrap()
            ),
            Some(json!({"enabled":true})),
            None,
            400,
        )
        .await?;
    relay.stop().await?;
    relay.start().await?;
    relay
        .api
        .deny("/api/groups", None, Some(token), 401)
        .await?;
    assert!(
        !relay
            .api
            .get_as("/api/groups", other_token)
            .await?
            .as_array()
            .unwrap()
            .is_empty()
    );
    Ok(())
}
pub async fn revocation_cancels_only_owned_queued_work_and_never_retries_submitted_send()
-> Result<()> {
    let mut relay = Relay::new("forwarding").await?;
    relay.api.select(&["fixture-a"]).await?;
    let owner = relay
        .api
        .key(json!({"groups":["fixture-a"],"can_send":true}))
        .await?;
    let other = relay
        .api
        .key(json!({"groups":["fixture-a"],"can_send":true}))
        .await?;
    let token = owner["token"].as_str().unwrap();
    let other_token = other["token"].as_str().unwrap();
    let first = message("fixture-a", "hold pending acknowledgement");
    let queued = message("fixture-a", "must not submit");
    let untouched = message("fixture-a", "other client queue");
    relay
        .api
        .post_as("/api/messages", first.clone(), token)
        .await?;
    relay
        .state(first["id"].as_str().unwrap(), "sending")
        .await?;
    relay
        .api
        .post_as("/api/messages", queued.clone(), token)
        .await?;
    relay
        .api
        .post_as("/api/messages", untouched.clone(), other_token)
        .await?;
    relay
        .api
        .deny("/api/messages", Some(first.clone()), Some(other_token), 400)
        .await?;
    relay.api.update(&owner, json!({"can_send":false})).await?;
    for (row, status) in [
        (&queued, "failed"),
        (&untouched, "queued"),
        (&first, "sending"),
    ] {
        assert_eq!(
            relay.stored(row["id"].as_str().unwrap())?.unwrap()["status"],
            status
        );
    }
    let mut spoof = message("fixture-a", "blocked spoof");
    spoof["client_id"] = other["client"]["id"].clone();
    relay
        .api
        .deny("/api/messages", Some(spoof), Some(token), 403)
        .await?;
    relay.api.select(&[]).await?;
    relay.state(first["id"].as_str().unwrap(), "sent").await?;
    relay.api.select(&["fixture-a"]).await?;
    relay.api.update(&owner, json!({"can_send":true})).await?;
    assert_eq!(
        relay
            .api
            .post_as("/api/messages", first.clone(), token)
            .await?["status"],
        "sent"
    );
    relay.stop().await?;
    relay.start().await?;
    assert_eq!(
        relay
            .commands("send")?
            .iter()
            .map(|c| c["message"]["id"].clone())
            .collect::<Vec<_>>(),
        [first["id"].clone()]
    );
    Ok(())
}
