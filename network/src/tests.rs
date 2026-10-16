use super::*;
use tokio::io::AsyncReadExt;

fn configured() -> Settings {
    Settings {
        enabled: true,
        relay_url: "https://relay.example.invalid:54443/".into(),
        relay_token: "synthetic-relay-token".into(),
        relay_ca: String::new(),
        relay_quic_port: 54443,
    }
}

#[test]
fn relay_configuration_rejects_downgrades_credentials_and_bad_ca() {
    assert!(Settings::default().validate().is_ok());
    let mut settings = configured();
    assert!(settings.validate().is_ok());
    for bad in [
        "http://relay.example.invalid:54443/",
        "ftp://127.0.0.1/",
        "https://a:b@relay.invalid/",
        "https://relay.invalid/private",
        "https://relay.invalid/?token=x",
        "https://relay.invalid/#x",
        " https://relay.invalid/",
        "https://relay.invalid:0/",
    ] {
        settings.relay_url = bad.into();
        assert!(settings.validate().is_err(), "accepted {bad}");
    }
    settings.relay_url = "http://127.0.0.1:12345/".into();
    assert!(settings.validate().is_ok());
    settings.relay_token = "injected\r\nheader".into();
    assert!(settings.validate().is_err());
    settings = configured();
    settings.relay_quic_port = 0;
    assert!(settings.validate().is_err());
    settings = configured();
    settings.relay_ca = "-----BEGIN CERTIFICATE-----\nYmFk\n-----END CERTIFICATE-----".into();
    assert!(settings.validate().is_err());
    settings.relay_ca = "-----BEGIN PRIVATE KEY-----\nYmFk\n-----END PRIVATE KEY-----".into();
    assert!(settings.validate().is_err());
    settings.relay_ca = "x".repeat(16 * 1024 + 1);
    assert!(settings.validate().is_err());
    settings = configured();
    settings.relay_token = "x".repeat(513);
    assert!(settings.validate().is_err());
    assert!(!format!("{:?}", configured()).contains("synthetic-relay-token"));
    assert!(serde_json::from_str::<Settings>(r#"{"unknown":true}"#).is_err());
}

#[test]
fn stable_identity_is_create_only_and_corruption_never_rotates_it() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("identity.key");
    let first = identity_key(&path).unwrap();
    let saved = fs::read(&path).unwrap();
    assert_eq!(saved.len(), 32);
    assert_eq!(first.public(), identity_key(&path).unwrap().public());
    assert_eq!(saved, fs::read(&path).unwrap());
    assert!(create_key(&path).is_err());
    fs::write(&path, b"partial").unwrap();
    assert!(identity_key(&path).is_err());
    assert_eq!(fs::read(&path).unwrap(), b"partial");
    assert!(identity_key(Path::new("relative.key")).is_err());
    #[cfg(unix)]
    {
        use std::os::unix::fs::{PermissionsExt, symlink};
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        let link = directory.path().join("link.key");
        symlink(&path, &link).unwrap();
        assert!(identity_key(&link).is_err());
    }
}

#[test]
fn only_nonzero_loopback_targets_are_permitted() {
    for allowed in ["127.0.0.1:443", "[::1]:443"] {
        assert!(validate_target(allowed.parse().unwrap()).is_ok());
    }
    for refused in ["127.0.0.1:0", "0.0.0.0:443", "192.168.1.1:443", "[::]:443"] {
        assert!(validate_target(refused.parse().unwrap()).is_err());
    }
}

async fn test_endpoint() -> Endpoint {
    Endpoint::builder(presets::Minimal)
        .relay_mode(RelayMode::Disabled)
        .alpns(vec![ALPN.to_vec()])
        .transport_config(transport())
        .bind_addr("127.0.0.1:0")
        .unwrap()
        .bind()
        .await
        .unwrap()
}

fn direct_address(endpoint: &Endpoint) -> EndpointAddr {
    let address = endpoint
        .bound_sockets()
        .into_iter()
        .find(SocketAddr::is_ipv4)
        .unwrap();
    EndpointAddr::new(endpoint.id()).with_ip_addr(address)
}

#[tokio::test]
async fn fixed_identity_cannot_be_replaced_by_a_reachable_wrong_peer() {
    let server = test_endpoint().await;
    let client = test_endpoint().await;
    let wrong = SecretKey::generate().public();
    let address = direct_address(&server).ip_addrs().next().copied().unwrap();
    let accept = tokio::spawn({
        let server = server.clone();
        async move {
            if let Some(incoming) = server.accept().await {
                let _ = incoming.await;
            }
        }
    });
    let result = timeout(
        Duration::from_secs(5),
        client.connect(EndpointAddr::new(wrong).with_ip_addr(address), ALPN),
    )
    .await;
    assert!(
        matches!(result, Ok(Err(_))),
        "a reachable endpoint must not satisfy a different public identity"
    );
    accept.abort();
    client.close().await;
    server.close().await;
}

#[tokio::test]
async fn quic_streams_preserve_bytes_half_close_and_reuse_one_connection() {
    let target = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let target_address = target.local_addr().unwrap();
    let echo = tokio::spawn(async move {
        let mut jobs = JoinSet::new();
        for _ in 0..2 {
            let (mut socket, _) = target.accept().await.unwrap();
            jobs.spawn(async move {
                let (mut read, mut write) = socket.split();
                tokio::io::copy(&mut read, &mut write).await.unwrap();
                write.shutdown().await.unwrap();
            });
        }
        while let Some(result) = jobs.join_next().await {
            result.unwrap();
        }
    });
    let server = test_endpoint().await;
    let endpoint = test_endpoint().await;
    let (shutdown, stopped) = watch::channel(false);
    let serving = tokio::spawn(serve(server.clone(), target_address, stopped));
    let (events, _output) = mpsc::channel(32);
    let client = Arc::new(Client {
        endpoint: endpoint.clone(),
        remote: direct_address(&server),
        connection: Mutex::new(None),
        events,
    });
    let mut previous = None;
    for sequence in 0..2 {
        let ingress = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let mut socket = TcpStream::connect(ingress.local_addr().unwrap())
            .await
            .unwrap();
        let (local, _) = ingress.accept().await.unwrap();
        let job = tokio::spawn({
            let client = client.clone();
            async move { client.tunnel(local).await }
        });
        let mut request = vec![22, 3, 3, 0, 1, 1]; // Synthetic TLS-shaped data, never a real service request.
        request.extend((0..200_000).map(|index| ((index + sequence) % 251) as u8));
        let expected = request.clone();
        let (mut read, mut write) = socket.split();
        let (sent, response) = tokio::join!(
            async {
                write.write_all(&request).await.unwrap();
                write.shutdown().await.unwrap();
            },
            async {
                let mut response = Vec::new();
                timeout(Duration::from_secs(8), read.read_to_end(&mut response))
                    .await
                    .unwrap()
                    .unwrap();
                response
            }
        );
        let _ = sent;
        assert_eq!(response, expected);
        job.await.unwrap().unwrap();
        let id = client.connection.lock().await.as_ref().unwrap().stable_id();
        if let Some(previous) = previous {
            assert_eq!(previous, id);
        }
        previous = Some(id);
    }
    echo.await.unwrap();
    shutdown.send(true).unwrap();
    serving.await.unwrap().unwrap();
    endpoint.close().await;
}

#[tokio::test]
async fn a_failed_target_stream_does_not_reconnect_or_replay_bytes() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let (mut peer, tunnel) = tokio::io::duplex(1024);
    let proxy = tokio::spawn(forward_to_target(tunnel, address));
    peer.write_all(&[22, 3, 3, 0, 1, 1, 99, 98, 97])
        .await
        .unwrap();
    let (mut accepted, _) = listener.accept().await.unwrap();
    let mut prefix = [0; 9];
    accepted.read_exact(&mut prefix).await.unwrap();
    assert_eq!(prefix, [22, 3, 3, 0, 1, 1, 99, 98, 97]);
    drop(accepted);
    peer.shutdown().await.unwrap();
    let _ = timeout(Duration::from_secs(2), proxy)
        .await
        .unwrap()
        .unwrap();
    assert!(
        timeout(Duration::from_millis(100), listener.accept())
            .await
            .is_err()
    );

    let (mut plaintext, tunnel) = tokio::io::duplex(1024);
    let rejected = tokio::spawn(forward_to_target(tunnel, address));
    plaintext.write_all(b"POST / HTTP/1.1\r\n").await.unwrap();
    assert!(rejected.await.unwrap().is_err());
    assert!(
        timeout(Duration::from_millis(100), listener.accept())
            .await
            .is_err()
    );
}

#[tokio::test]
async fn shutdown_closes_waiting_streams_without_reaching_the_target() {
    let target = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let server = test_endpoint().await;
    let client = test_endpoint().await;
    let (shutdown, stopped) = watch::channel(false);
    let serving = tokio::spawn(serve(server.clone(), target.local_addr().unwrap(), stopped));
    let connection = client.connect(direct_address(&server), ALPN).await.unwrap();
    let (mut send, _recv) = connection.open_bi().await.unwrap();
    send.write_all(&[22]).await.unwrap(); // An incomplete TLS header must not open a TCP target.
    shutdown.send(true).unwrap();
    timeout(Duration::from_secs(4), serving)
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    timeout(Duration::from_secs(2), connection.closed())
        .await
        .unwrap();
    assert!(
        timeout(Duration::from_millis(100), target.accept())
            .await
            .is_err()
    );
    client.close().await;
}

#[tokio::test]
async fn a_failed_stream_preserves_helper_listener_other_streams_and_future_connections() {
    let target = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let target_address = target.local_addr().unwrap();
    let echo = tokio::spawn(async move {
        let mut jobs = JoinSet::new();
        for _ in 0..2 {
            let (mut socket, _) = target.accept().await.unwrap();
            jobs.spawn(async move {
                let (mut read, mut write) = socket.split();
                tokio::io::copy(&mut read, &mut write).await.unwrap();
                write.shutdown().await.unwrap();
            });
        }
        while let Some(job) = jobs.join_next().await {
            job.unwrap();
        }
    });
    let server = test_endpoint().await;
    let endpoint = test_endpoint().await;
    let (shutdown, stopped) = watch::channel(false);
    let serving = tokio::spawn(serve(server.clone(), target_address, stopped.clone()));
    let (events, mut output) = mpsc::channel(32);
    let helper = tokio::spawn(client_with_address(
        endpoint,
        direct_address(&server),
        stopped,
        events,
    ));
    let address = loop {
        match timeout(Duration::from_secs(5), output.recv())
            .await
            .unwrap()
            .unwrap()
        {
            Event::Ready { url } => {
                let url = url::Url::parse(&url).unwrap();
                break SocketAddr::from(([127, 0, 0, 1], url.port().unwrap()));
            }
            Event::Error { .. } => panic!("helper could not initialize"),
            _ => {}
        }
    };
    let mut live = TcpStream::connect(address).await.unwrap();
    let header = [22, 3, 3, 0, 1, 1];
    live.write_all(&header).await.unwrap();
    let mut echoed = [0; 6];
    timeout(Duration::from_secs(5), live.read_exact(&mut echoed))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(echoed, header);

    let mut cancelled = TcpStream::connect(address).await.unwrap();
    cancelled.write_all(&[22]).await.unwrap();
    cancelled.shutdown().await.unwrap();
    let mut discarded = Vec::new();
    timeout(
        Duration::from_secs(2),
        cancelled.read_to_end(&mut discarded),
    )
    .await
    .unwrap()
    .unwrap();
    live.write_all(&[42]).await.unwrap();
    assert_eq!(
        timeout(Duration::from_secs(2), live.read_u8())
            .await
            .unwrap()
            .unwrap(),
        42
    );

    let mut later = TcpStream::connect(address).await.unwrap();
    later.write_all(&header).await.unwrap();
    timeout(Duration::from_secs(2), later.read_exact(&mut echoed))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(echoed, header);
    later.shutdown().await.unwrap();
    later.read_to_end(&mut discarded).await.unwrap();
    live.shutdown().await.unwrap();
    live.read_to_end(&mut discarded).await.unwrap();
    echo.await.unwrap();
    shutdown.send(true).unwrap();
    helper.await.unwrap().unwrap();
    serving.await.unwrap().unwrap();
    while let Ok(event) = output.try_recv() {
        assert!(
            !matches!(event, Event::Error { .. }),
            "a cancelled stream is never a fatal event"
        );
    }
}
