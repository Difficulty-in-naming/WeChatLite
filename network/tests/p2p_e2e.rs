//! Real helper + private, in-process iroh relay + demo server. No WeChat or public relay.
//!
//! Build wechat-relay, then explicitly run these ignored tests with
//! WECHAT_RELAY_BINARY pointing at that binary. The helper is built by Cargo itself.

use anyhow::{Context, Result, bail, ensure};
use futures_util::{SinkExt, StreamExt};
use iroh_relay::server::{
    Access, AccessControl, CertConfig, ClientRequest, QuicConfig, RelayConfig, Server,
    ServerConfig, TlsConfig,
};
use reqwest::{Client, Method, StatusCode};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs::{self, File},
    net::{Ipv4Addr, TcpListener},
    path::{Path, PathBuf},
    process::Stdio,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};
use tempfile::TempDir;
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    net::TcpStream,
    process::{Child, Command},
    sync::watch,
    task::JoinHandle,
    time::{Instant, sleep, timeout},
};
use tokio_tungstenite::{
    Connector, MaybeTlsStream, WebSocketStream, connect_async_tls_with_config,
    tungstenite::{
        Message,
        client::IntoClientRequest,
        http::{HeaderValue, header::AUTHORIZATION},
    },
};

type Socket = WebSocketStream<MaybeTlsStream<TcpStream>>;
const HTTP_TIMEOUT: Duration = Duration::from_secs(40);
const HELPER_LINE_LIMIT: usize = 16 * 1024;

struct TokenGate {
    token: String,
    admitted: Arc<AtomicUsize>,
}

impl std::fmt::Debug for TokenGate {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SyntheticTokenGate").finish_non_exhaustive()
    }
}

impl AccessControl for TokenGate {
    async fn on_connect(&self, request: &ClientRequest) -> Access {
        if request.auth_token().as_deref() == Some(self.token.as_str()) {
            self.admitted.fetch_add(1, Ordering::Relaxed);
            Access::Allow
        } else {
            Access::Deny { reason: None }
        }
    }
}

fn command(program: impl AsRef<std::ffi::OsStr>) -> Command {
    let mut command = Command::new(program);
    command.kill_on_drop(true);
    for name in [
        "HTTP_PROXY",
        "HTTPS_PROXY",
        "ALL_PROXY",
        "http_proxy",
        "https_proxy",
        "all_proxy",
    ] {
        command.env_remove(name);
    }
    command.env("NO_PROXY", "*").env("no_proxy", "*");
    #[cfg(windows)]
    command.creation_flags(0x0800_0000); // Do not open a console for owned test children.
    command
}

fn secret() -> String {
    // Random fixture-only credentials, never a saved configuration or cloud secret.
    iroh::SecretKey::generate().public().to_string()
}

fn tls(certificate: &[u8], key: &[u8]) -> Result<(rustls::ClientConfig, rustls::ServerConfig)> {
    let certificates = rustls_pemfile::certs(&mut certificate.as_ref())
        .collect::<std::result::Result<Vec<_>, _>>()?;
    ensure!(!certificates.is_empty(), "fixture certificate is empty");
    let private =
        rustls_pemfile::private_key(&mut key.as_ref())?.context("fixture private key is empty")?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut roots = rustls::RootCertStore::empty();
    for certificate in &certificates {
        roots.add(certificate.clone())?;
    }
    let client = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()?
        .with_root_certificates(roots)
        .with_no_client_auth();
    let server = rustls::ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()?
        .with_no_client_auth()
        .with_single_cert(certificates, private)?;
    Ok((client, server))
}

#[derive(Clone, Default)]
struct HelperEvents {
    ready: Option<String>,
    paths: BTreeSet<String>,
    failed: bool,
    ended: bool,
}

struct Helper {
    child: Child,
    events: watch::Receiver<HelperEvents>,
    reader: JoinHandle<Result<()>>,
}

impl Helper {
    async fn start(
        settings: &Value,
        server_id: &str,
        directory: &Path,
        relay_only: bool,
    ) -> Result<Self> {
        let mut child = command(env!("CARGO_BIN_EXE_wechat-link"))
            .arg("--client")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .context("start the Cargo-built P2P helper")?;
        let mut configuration = settings.clone();
        let object = configuration
            .as_object_mut()
            .context("fixture settings are not an object")?;
        object.remove("enabled");
        object.insert("server_id".into(), json!(server_id));
        object.insert("key_file".into(), json!(directory.join("client.key")));
        object.insert("relay_only".into(), json!(relay_only));
        let mut line = serde_json::to_vec(&configuration)?;
        line.push(b'\n');
        let input = child.stdin.as_mut().context("helper stdin is absent")?;
        input.write_all(&line).await?;
        input.flush().await?; // Keep stdin open until explicit ownership shutdown.

        let output = child.stdout.take().context("helper stdout is absent")?;
        let (events, receiver) = watch::channel(HelperEvents::default());
        let reader = tokio::spawn(async move {
            let mut output = BufReader::new(output);
            let result = async {
                loop {
                    let mut line = Vec::new();
                    loop {
                        let available = output.fill_buf().await?;
                        if available.is_empty() {
                            ensure!(line.is_empty(), "helper ended with a partial NDJSON line");
                            return Ok(());
                        }
                        let count = available
                            .iter()
                            .position(|byte| *byte == b'\n')
                            .map(|index| index + 1)
                            .unwrap_or(available.len());
                        ensure!(
                            line.len() + count <= HELPER_LINE_LIMIT,
                            "helper NDJSON line exceeds its bound"
                        );
                        line.extend_from_slice(&available[..count]);
                        output.consume(count);
                        if line.last() == Some(&b'\n') {
                            break;
                        }
                    }
                    let event: Value =
                        serde_json::from_slice(&line).context("helper returned invalid NDJSON")?;
                    match event["type"].as_str() {
                        Some("ready") => {
                            let url = event["url"]
                                .as_str()
                                .context("helper ready URL is missing")?;
                            let parsed = url::Url::parse(url)?;
                            ensure!(
                                parsed.scheme() == "https"
                                    && parsed.host_str() == Some("127.0.0.1")
                                    && parsed.port().is_some_and(|port| port != 0)
                                    && parsed.username().is_empty()
                                    && parsed.password().is_none()
                                    && parsed.query().is_none()
                                    && parsed.fragment().is_none(),
                                "helper ingress must remain a loopback HTTPS URL"
                            );
                            events.send_modify(|state| state.ready = Some(url.to_owned()));
                        }
                        Some("path") => {
                            let path = event["path"].as_str().context("helper path is missing")?;
                            ensure!(
                                path == "direct" || path == "relay",
                                "helper returned an unknown path"
                            );
                            events.send_modify(|state| {
                                state.paths.insert(path.to_owned());
                            });
                        }
                        Some("error") => bail!("helper reported a connection error"),
                        _ => bail!("helper returned an unknown event"),
                    }
                }
            }
            .await;
            events.send_modify(|state| {
                state.failed = result.is_err();
                state.ended = true;
            });
            result
        });
        let mut helper = Self {
            child,
            events: receiver,
            reader,
        };
        let state = timeout(
            Duration::from_secs(35),
            helper
                .events
                .wait_for(|state| state.ready.is_some() || state.failed || state.ended),
        )
        .await
        .context("helper ready timed out")?
        .context("helper output ended before ready")?
        .clone();
        ensure!(
            !state.failed && !state.ended && state.ready.is_some(),
            "helper did not become ready"
        );
        Ok(helper)
    }

    fn base(&self) -> Result<String> {
        self.events
            .borrow()
            .ready
            .clone()
            .context("helper ingress is not ready")
    }

    fn healthy(&mut self) -> Result<()> {
        ensure!(
            !self.events.borrow().failed,
            "helper output failed validation"
        );
        ensure!(
            self.child.try_wait()?.is_none(),
            "helper exited during a business request"
        );
        Ok(())
    }

    async fn wait_path(&mut self, relay_only: bool) -> Result<()> {
        let wanted = if relay_only { "relay" } else { "direct" };
        let state = timeout(
            Duration::from_secs(20),
            self.events
                .wait_for(|state| state.paths.contains(wanted) || state.failed || state.ended),
        )
        .await
        .context("expected local P2P route did not become active")??
        .clone();
        ensure!(
            state.paths.contains(wanted) && !state.failed,
            "helper failed before its expected route"
        );
        if relay_only {
            ensure!(
                !state.paths.contains("direct"),
                "relay-only diagnostic unexpectedly used IP transport"
            );
        }
        self.healthy()
    }

    async fn close(&mut self) -> Result<()> {
        drop(self.child.stdin.take());
        let status = timeout(Duration::from_secs(6), self.child.wait())
            .await
            .context("helper did not stop after owner stdin EOF")??;
        ensure!(status.success(), "helper ownership shutdown was not clean");
        timeout(Duration::from_secs(2), &mut self.reader)
            .await
            .context("helper stdout did not settle")???;
        Ok(())
    }
}

struct Fixture {
    server: Child,
    rendezvous: Option<Server>,
    helper: Option<Helper>,
    client: Client,
    tls: Arc<rustls::ClientConfig>,
    admin_base: String,
    admin_token: String,
    server_id: String,
    settings: Value,
    admitted: Arc<AtomicUsize>,
    directory: TempDir,
}

impl Fixture {
    async fn start() -> Result<Self> {
        let binary = PathBuf::from(
            std::env::var_os("WECHAT_RELAY_BINARY")
                .context("explicitly set WECHAT_RELAY_BINARY to a built demo-capable relay")?,
        );
        ensure!(
            binary.is_absolute() && binary.is_file(),
            "WECHAT_RELAY_BINARY must be an existing absolute file"
        );
        let directory = tempfile::tempdir()?;
        let output = timeout(
            Duration::from_secs(10),
            command(&binary)
                .args(["--make-cert", "--names", "localhost,127.0.0.1"])
                .current_dir(directory.path())
                .output(),
        )
        .await
        .context("fixture certificate creation timed out")??;
        ensure!(
            output.status.success(),
            "fixture certificate creation failed"
        );
        let certificate_path = directory.path().join(".state/tls/cert.pem");
        let key_path = certificate_path.with_file_name("key.pem");
        let certificate = fs::read(&certificate_path)?;
        let (client_tls, server_tls) = tls(&certificate, &fs::read(&key_path)?)?;
        let client = Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .tls_backend_preconfigured(client_tls.clone())
            .timeout(HTTP_TIMEOUT)
            .connect_timeout(Duration::from_secs(15))
            .build()?;
        let admitted = Arc::new(AtomicUsize::new(0));
        let relay_token = secret();
        let mut relay = RelayConfig::new((Ipv4Addr::LOCALHOST, 0));
        relay.tls = Some(TlsConfig::new(
            (Ipv4Addr::LOCALHOST, 0),
            CertConfig::Manual {
                server_config: server_tls.clone(),
            },
        ));
        relay.access = Arc::new(TokenGate {
            token: relay_token.clone(),
            admitted: admitted.clone(),
        });
        relay.key_cache_capacity = Some(32);
        let mut quic = QuicConfig::new((Ipv4Addr::LOCALHOST, 0));
        quic.server_config = Some(server_tls);
        let mut configuration = ServerConfig::default();
        configuration.relay = Some(relay);
        configuration.quic = Some(quic);
        let rendezvous = Server::spawn(configuration).await?;
        let relay_address = rendezvous
            .https_addr()
            .context("fixture HTTPS rendezvous is absent")?;
        let quic_address = rendezvous
            .quic_addr()
            .context("fixture QAD endpoint is absent")?;
        ensure!(
            relay_address.ip().is_loopback() && quic_address.ip().is_loopback(),
            "rendezvous escaped loopback"
        );
        let settings = json!({"enabled":true,"relay_url":format!("https://{relay_address}/"),
            "relay_token":relay_token,"relay_ca":String::from_utf8(certificate)?,
            "relay_quic_port":quic_address.port()});
        let reservation = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
        let address = reservation.local_addr()?;
        let admin_token = secret();
        let path = directory.path().join("server.json");
        fs::write(
            &path,
            serde_json::to_vec(&json!({"listen":address.to_string(),"token":admin_token,
            "data":directory.path().join("data"),"tls_cert":certificate_path,"tls_key":key_path,"p2p":settings}))?,
        )?;
        drop(reservation);
        let log = File::create(directory.path().join("demo.log"))?;
        let server = command(binary)
            .arg("--config")
            .arg(path)
            .arg("--demo")
            .current_dir(directory.path())
            .stdin(Stdio::null())
            .stdout(Stdio::from(log.try_clone()?))
            .stderr(Stdio::from(log))
            .spawn()?;
        let mut fixture = Self {
            server,
            rendezvous: Some(rendezvous),
            helper: None,
            client,
            tls: Arc::new(client_tls),
            admin_base: format!("https://{address}"),
            admin_token,
            server_id: String::new(),
            settings,
            admitted,
            directory,
        };
        let ready = fixture.wait_ready().await;
        if let Err(error) = ready {
            let _ = fixture.close().await;
            return Err(error);
        }
        Ok(fixture)
    }

    async fn wait_ready(&mut self) -> Result<()> {
        let end = Instant::now() + Duration::from_secs(20);
        while Instant::now() < end {
            ensure!(
                self.server.try_wait()?.is_none(),
                "demo relay exited before becoming ready"
            );
            if let Ok(value) = self
                .api(&self.admin_base, &self.admin_token, "/api/status", None)
                .await
            {
                if value["ready"] == true {
                    let state = self
                        .api(&self.admin_base, &self.admin_token, "/api/local/p2p", None)
                        .await?;
                    ensure!(
                        state["enabled"] == true && state["status"] == "listening",
                        "demo P2P endpoint is not listening"
                    );
                    self.server_id = state["server_id"]
                        .as_str()
                        .context("demo public identity is absent")?
                        .to_owned();
                    return Ok(());
                }
            }
            sleep(Duration::from_millis(50)).await;
        }
        bail!("demo relay did not become ready")
    }

    async fn response(
        &self,
        base: &str,
        token: &str,
        path: &str,
        value: Option<&Value>,
    ) -> Result<reqwest::Response> {
        let mut request = self
            .client
            .request(
                if value.is_some() {
                    Method::POST
                } else {
                    Method::GET
                },
                format!("{base}{path}"),
            )
            .bearer_auth(token);
        if let Some(value) = value {
            request = request.json(value);
        }
        request.send().await.context("fixture HTTPS request failed")
    }

    async fn api(
        &self,
        base: &str,
        token: &str,
        path: &str,
        value: Option<&Value>,
    ) -> Result<Value> {
        let response = self.response(base, token, path, value).await?;
        ensure!(
            response.status().is_success(),
            "fixture HTTPS request returned {} for {path}",
            response.status()
        );
        response
            .json()
            .await
            .context("fixture HTTPS response was not JSON")
    }

    async fn websocket(&mut self, token: &str, after: i64) -> Result<Socket> {
        let base = self.helper.as_ref().context("helper is absent")?.base()?;
        for attempt in 0..2 {
            let mut request = format!(
                "{}/api/events?after={after}",
                base.replacen("https://", "wss://", 1)
            )
            .into_client_request()?;
            request.headers_mut().insert(
                AUTHORIZATION,
                HeaderValue::from_str(&format!("Bearer {token}"))?,
            );
            match timeout(
                Duration::from_secs(20),
                connect_async_tls_with_config(
                    request,
                    None,
                    false,
                    Some(Connector::Rustls(self.tls.clone())),
                ),
            )
            .await
            {
                Ok(Ok((socket, _response))) => return Ok(socket),
                Ok(Err(_)) => bail!("fixture WSS TLS/upgrade failed"),
                Err(_) if attempt == 0 => {
                    // Retry only the safe WebSocket GET; business POSTs are never replayed here.
                    self.helper
                        .as_mut()
                        .context("helper is absent")?
                        .healthy()?;
                    ensure!(
                        self.api(&base, token, "/api/status", None).await?["ready"] == true,
                        "same helper stopped serving after an initial WSS timeout"
                    );
                    sleep(Duration::from_secs(1)).await;
                }
                Err(_) => bail!("fixture WSS upgrade timed out twice"),
            }
        }
        unreachable!()
    }

    async fn close(&mut self) -> Result<()> {
        let mut failures = Vec::new();
        if let Some(mut helper) = self.helper.take() {
            if helper.close().await.is_err() {
                failures.push("helper did not stop cleanly");
            }
        }
        if self.server.try_wait()?.is_none() {
            let stopped = self
                .api(
                    &self.admin_base,
                    &self.admin_token,
                    "/api/shutdown",
                    Some(&json!({})),
                )
                .await;
            let exit = timeout(Duration::from_secs(6), self.server.wait()).await;
            if !matches!(exit, Ok(Ok(status)) if status.success()) {
                let _ = self.server.kill().await;
                let _ = self.server.wait().await;
                failures.push("demo relay did not stop cleanly");
            } else if stopped.is_err() {
                failures.push("demo relay shutdown endpoint failed");
            }
        }
        if let Some(relay) = self.rendezvous.take() {
            if !matches!(
                timeout(Duration::from_secs(5), relay.shutdown()).await,
                Ok(Ok(()))
            ) {
                failures.push("private rendezvous did not stop cleanly");
            }
        }
        ensure!(failures.is_empty(), "{}", failures.join("; "));
        Ok(())
    }
}

async fn event(socket: &mut Socket) -> Result<Value> {
    loop {
        match timeout(Duration::from_secs(15), socket.next())
            .await
            .context("WSS event timed out")?
        {
            Some(Ok(Message::Text(text))) => {
                ensure!(text.len() <= 1024 * 1024, "WSS event exceeds fixture bound");
                return serde_json::from_str(&text).context("WSS event was not JSON");
            }
            Some(Ok(Message::Ping(data))) => socket.send(Message::Pong(data)).await?,
            Some(Ok(Message::Pong(_))) => {}
            _ => bail!("WSS closed before its expected event"),
        }
    }
}

async fn synced(socket: &mut Socket) -> Result<i64> {
    let mut cursor = 0;
    for _ in 0..256 {
        let value = event(socket).await?;
        cursor = cursor.max(value["seq"].as_i64().unwrap_or(0));
        if value["type"] == "synced" {
            return Ok(cursor);
        }
    }
    bail!("WSS replay did not end within its fixture bound")
}

fn matching_media_files(directory: &Path) -> Result<usize> {
    let mut count = 0;
    for day in fs::read_dir(directory)? {
        let day = day?;
        if day.file_type()?.is_dir() {
            for file in fs::read_dir(day.path())? {
                if file?
                    .file_name()
                    .to_string_lossy()
                    .ends_with("_透明传输.bin")
                {
                    count += 1;
                }
            }
        }
    }
    Ok(count)
}

async fn contract(fixture: &mut Fixture, relay_only: bool) -> Result<()> {
    let credential = fixture.api(&fixture.admin_base, &fixture.admin_token, "/api/local/clients", Some(&json!({
        "name":"Isolated Rust P2P fixture","groups":["demo-design"],"can_send":true,"can_download":true
    }))).await?;
    let token = credential["token"]
        .as_str()
        .context("independent client key is absent")?
        .to_owned();
    ensure!(
        token != fixture.admin_token,
        "client credential unexpectedly equals administrator credential"
    );
    fixture.helper = Some(
        Helper::start(
            &fixture.settings,
            &fixture.server_id,
            fixture.directory.path(),
            relay_only,
        )
        .await?,
    );
    let base = fixture
        .helper
        .as_ref()
        .context("helper is absent")?
        .base()?;
    let groups = fixture.api(&base, &token, "/api/groups", None).await?;
    let groups = groups
        .as_array()
        .context("group response was not an array")?;
    ensure!(
        groups.len() == 1 && groups[0]["id"] == "demo-design",
        "tunneled group permissions changed"
    );
    ensure!(
        fixture
            .response(&base, &fixture.admin_token, "/api/groups", None)
            .await?
            .status()
            == StatusCode::FORBIDDEN,
        "tunnel ingress accepted a local administrator key"
    );
    for path in [
        "/api/local/clients",
        "/api/local/forwarding",
        "/api/local/p2p",
        "/api/shutdown",
    ] {
        ensure!(
            fixture.response(&base, &token, path, None).await?.status() == StatusCode::NOT_FOUND,
            "tunnel ingress mounted a management endpoint: {path}"
        );
    }
    ensure!(
        fixture
            .response(&base, &token, "/api/messages?group=demo-dev", None)
            .await?
            .status()
            == StatusCode::FORBIDDEN,
        "tunnel ingress exposed an unauthorized group"
    );
    let raw: Vec<u8> = (0..8 * 1024 * 1024 + 13)
        .map(|index| ((index * 31 + 17) % 251) as u8)
        .collect();
    let expected = format!("{:x}", Sha256::digest(&raw));
    let media_directory = fixture.directory.path().join("data/media");
    let before = matching_media_files(&media_directory)?;
    let mut upload = url::Url::parse(&format!("{base}/api/media"))?;
    upload
        .query_pairs_mut()
        .append_pair("group", "demo-design")
        .append_pair("name", "透明传输.bin");
    let response = fixture
        .client
        .post(upload)
        .bearer_auth(&token)
        .header("Content-Type", "application/octet-stream")
        .body(raw.clone())
        .send()
        .await?;
    ensure!(
        response.status().is_success(),
        "tunneled attachment upload failed"
    );
    let media: Value = response.json().await?;
    ensure!(
        media["sha256"] == expected && media.get("path").is_none(),
        "attachment checksum or public shape changed"
    );
    let id = media["id"]
        .as_str()
        .context("uploaded attachment id is absent")?;
    let response = fixture
        .response(&base, &token, &format!("/api/media/{id}"), None)
        .await?;
    ensure!(
        response.status().is_success() && response.bytes().await?.as_ref() == raw.as_slice(),
        "tunneled attachment download changed its bytes"
    );
    ensure!(
        matching_media_files(&media_directory)? == before + 1,
        "attachment upload created duplicate server files"
    );
    let mut socket = fixture.websocket(&token, 0).await?;
    let cursor = synced(&mut socket).await?;
    socket.close(None).await?;
    drop(socket);
    let message_id = if relay_only {
        "00000000-0000-4000-8000-000000000002"
    } else {
        "00000000-0000-4000-8000-000000000001"
    };
    let message = json!({"id":message_id,"group":"demo-design","client_id":"rust-p2p-fixture",
        "sender":"Offline demonstration","text":"@林小满 isolated tunnel contract","media":[id],
        "mentions":[{"id":"lin","name":"林小满"}]});
    fixture
        .api(&base, &token, "/api/messages", Some(&message))
        .await?;
    let mut socket = fixture.websocket(&token, cursor).await?;
    let mut sent = false;
    let mut last_sequence = cursor;
    for _ in 0..256 {
        let value = event(&mut socket).await?;
        if value["type"] == "message" {
            let sequence = value["seq"]
                .as_i64()
                .context("message event has no durable sequence")?;
            ensure!(
                sequence > last_sequence,
                "tunneled WSS replay repeated or reordered an event"
            );
            last_sequence = sequence;
            ensure!(
                value["message"]["group"] == "demo-design",
                "WSS replay crossed the client's group boundary"
            );
            if value["message"]["id"] == message_id && value["message"]["status"] == "sent" {
                ensure!(
                    value["message"]["mentions"][0]["id"] == "lin"
                        && value["message"]["media"][0]["id"] == id,
                    "confirmation lost the real mention or attachment association"
                );
                sent = true;
                break;
            }
        }
    }
    ensure!(sent, "tunneled send was not confirmed after WSS reconnect");
    fixture
        .api(&base, &token, "/api/messages", Some(&message))
        .await?;
    let history = fixture
        .api(&base, &token, "/api/messages?group=demo-design", None)
        .await?;
    ensure!(
        history
            .as_array()
            .context("message history is not an array")?
            .iter()
            .filter(|row| row["id"] == message_id)
            .count()
            == 1,
        "same UUID was submitted twice through the tunnel"
    );
    let client_id = credential["client"]["id"]
        .as_str()
        .context("independent client id is absent")?;
    fixture
        .api(
            &fixture.admin_base,
            &fixture.admin_token,
            &format!("/api/local/clients/{client_id}/revoke"),
            Some(&json!({})),
        )
        .await?;
    let closed = timeout(Duration::from_secs(10), async {
        for _ in 0..32 {
            match socket.next().await {
                Some(Ok(Message::Close(_))) | Some(Err(_)) | None => return true,
                Some(Ok(Message::Ping(bytes))) => {
                    let _ = socket.send(Message::Pong(bytes)).await;
                }
                _ => {}
            }
        }
        false
    })
    .await
    .context("revoked client WSS did not close")?;
    ensure!(closed, "revoked WSS remained live");
    ensure!(
        fixture
            .response(&base, &token, "/api/status", None)
            .await?
            .status()
            == StatusCode::UNAUTHORIZED,
        "revoked client key continued to authenticate over P2P"
    );
    let helper = fixture.helper.as_mut().context("helper is absent")?;
    helper.wait_path(relay_only).await?;
    ensure!(
        fixture.admitted.load(Ordering::Relaxed) >= 2,
        "shared-token rendezvous did not authenticate both endpoints"
    );
    Ok(())
}

async fn exercise(relay_only: bool) -> Result<()> {
    let mut fixture = Fixture::start().await?;
    let result = timeout(Duration::from_secs(150), contract(&mut fixture, relay_only))
        .await
        .context("local P2P end-to-end contract timed out")
        .and_then(|value| value);
    let cleanup = fixture.close().await;
    result?;
    cleanup
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore = "requires explicitly built WECHAT_RELAY_BINARY; all networking stays local"]
async fn normal_path_keeps_tls_permissions_files_events_and_receipts() -> Result<()> {
    exercise(false).await
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore = "requires explicitly built WECHAT_RELAY_BINARY; all networking stays local"]
async fn relay_only_path_keeps_the_same_application_contract() -> Result<()> {
    exercise(true).await
}
