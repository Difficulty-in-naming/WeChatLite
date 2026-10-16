pub mod adapter;
pub mod cases;

use anyhow::{Context, Result, bail, ensure};
use futures_util::{SinkExt, StreamExt};
use reqwest::{Client, Method};
use rusqlite::{Connection, OptionalExtension, params};
use rustls::{
    ClientConfig, RootCertStore,
    pki_types::{CertificateDer, pem::PemObject},
};
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    future::Future,
    net::TcpListener,
    path::PathBuf,
    process::{Child, Command, Output, Stdio},
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::net::TcpStream;
use tokio_tungstenite::{
    Connector, MaybeTlsStream, WebSocketStream,
    tungstenite::{Message, client::IntoClientRequest},
};

pub type Ws = WebSocketStream<MaybeTlsStream<TcpStream>>;
pub const WAIT: Duration = Duration::from_secs(6);

pub fn binary() -> PathBuf {
    std::env::var_os("WECHAT_RELAY_BINARY")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_BIN_EXE_wechat-relay")))
}
pub fn child_command(path: impl AsRef<std::ffi::OsStr>) -> Command {
    let mut command = Command::new(path);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    command
}
pub async fn run_output(mut command: Command) -> Result<Output> {
    command.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut command = tokio::process::Command::from(command);
    command.kill_on_drop(true);
    Ok(tokio::time::timeout(WAIT, command.output()).await??)
}
pub fn now() -> i64 {
    chrono::Utc::now().timestamp_millis()
}
pub fn message(group: &str, text: &str) -> Value {
    json!({"id":uuid::Uuid::new_v4().to_string(),"group":group,"client_id":"fixture-client","sender":"测试用户","text":text,"media":[],"mentions":[]})
}
pub fn ids(rows: &Value) -> BTreeSet<String> {
    rows.as_array()
        .unwrap()
        .iter()
        .map(|r| r["id"].as_str().unwrap().to_owned())
        .collect()
}
pub fn query(path: &str, fields: &[(&str, String)]) -> String {
    let mut result = reqwest::Url::parse("http://fixture.invalid").unwrap();
    result
        .query_pairs_mut()
        .extend_pairs(fields.iter().map(|(k, v)| (*k, v.as_str())));
    format!("{path}?{}", result.query().unwrap_or(""))
}

#[derive(Clone)]
pub struct Api {
    pub base: String,
    pub token: String,
    pub client: Client,
    tls: Option<Arc<ClientConfig>>,
}
impl Api {
    pub async fn raw(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
        bytes: Option<Vec<u8>>,
        token: Option<&str>,
    ) -> Result<reqwest::Response> {
        let mut request = self
            .client
            .request(method, format!("{}{path}", self.base))
            .bearer_auth(token.unwrap_or(&self.token));
        if let Some(body) = body {
            request = request.json(&body);
        }
        if let Some(bytes) = bytes {
            request = request
                .header("Content-Type", "application/octet-stream")
                .body(bytes);
        }
        Ok(request.send().await?)
    }
    pub async fn get(&self, path: &str) -> Result<Value> {
        self.value(Method::GET, path, None, None).await
    }
    pub async fn get_as(&self, path: &str, token: &str) -> Result<Value> {
        self.value(Method::GET, path, None, Some(token)).await
    }
    pub async fn post(&self, path: &str, value: Value) -> Result<Value> {
        self.value(Method::POST, path, Some(value), None).await
    }
    pub async fn post_as(&self, path: &str, value: Value, token: &str) -> Result<Value> {
        self.value(Method::POST, path, Some(value), Some(token))
            .await
    }
    pub async fn value(
        &self,
        method: Method,
        path: &str,
        body: Option<Value>,
        token: Option<&str>,
    ) -> Result<Value> {
        let response = self.raw(method, path, body, None, token).await?;
        let status = response.status();
        let value: Value = response.json().await?;
        ensure!(
            status.is_success(),
            "fixture HTTP {path} returned {status}: {value}"
        );
        Ok(value)
    }
    pub async fn deny(
        &self,
        path: &str,
        body: Option<Value>,
        token: Option<&str>,
        code: u16,
    ) -> Result<()> {
        let response = self
            .raw(
                if body.is_some() {
                    Method::POST
                } else {
                    Method::GET
                },
                path,
                body,
                None,
                token,
            )
            .await?;
        ensure!(
            response.status().as_u16() == code,
            "{path}: expected {code}, got {}",
            response.status()
        );
        Ok(())
    }
    pub async fn upload(
        &self,
        group: &str,
        name: &str,
        bytes: Vec<u8>,
        token: Option<&str>,
    ) -> Result<Value> {
        let path = query(
            "/api/media",
            &[("group", group.into()), ("name", name.into())],
        );
        let response = self
            .raw(Method::POST, &path, None, Some(bytes), token)
            .await?;
        ensure!(
            response.status().is_success(),
            "fixture upload failed: {}",
            response.status()
        );
        Ok(response.json().await?)
    }
    pub async fn download(&self, id: &str, token: Option<&str>) -> Result<Vec<u8>> {
        let response = self
            .raw(Method::GET, &format!("/api/media/{id}"), None, None, token)
            .await?;
        ensure!(
            response.status().is_success(),
            "fixture download failed: {}",
            response.status()
        );
        Ok(response.bytes().await?.to_vec())
    }
    pub async fn ws(&self, after: i64, token: Option<&str>) -> Result<Ws> {
        let url = format!(
            "{}/api/events?after={after}",
            self.base.replacen("http", "ws", 1)
        );
        let mut request = url.into_client_request()?;
        request.headers_mut().insert(
            "Authorization",
            format!("Bearer {}", token.unwrap_or(&self.token)).parse()?,
        );
        let (socket, _) = tokio::time::timeout(
            WAIT,
            tokio_tungstenite::connect_async_tls_with_config(
                request,
                None,
                false,
                self.tls.clone().map(Connector::Rustls),
            ),
        )
        .await??;
        Ok(socket)
    }
    pub async fn select(&self, groups: &[&str]) -> Result<Value> {
        self.post("/api/local/forwarding", json!({"selected":groups}))
            .await
    }
    pub async fn key(&self, fields: Value) -> Result<Value> {
        let mut request = json!({"name":"Independent fixture client"});
        request
            .as_object_mut()
            .unwrap()
            .extend(fields.as_object().unwrap().clone());
        self.post("/api/local/clients", request).await
    }
    pub async fn update(&self, key: &Value, fields: Value) -> Result<Value> {
        self.post(
            &format!(
                "/api/local/clients/{}",
                key["client"]["id"].as_str().unwrap()
            ),
            fields,
        )
        .await
    }
}

pub async fn recv(socket: &mut Ws) -> Result<Value> {
    tokio::time::timeout(WAIT, async {
        loop {
            match socket.next().await.context("fixture websocket closed")?? {
                Message::Text(text) => return Ok(serde_json::from_str(&text)?),
                Message::Ping(bytes) => socket.send(Message::Pong(bytes)).await?,
                Message::Close(_) => bail!("fixture websocket closed"),
                _ => {}
            }
        }
    })
    .await?
}
pub async fn sync(socket: &mut Ws) -> Result<Vec<Value>> {
    let mut events = Vec::new();
    loop {
        let event = recv(socket).await?;
        let done = event["type"] == "synced";
        events.push(event);
        if done {
            return Ok(events);
        }
    }
}
pub async fn sent(socket: &mut Ws, id: &str) -> Result<Vec<Value>> {
    let mut events = Vec::new();
    loop {
        let event = recv(socket).await?;
        if event["type"] == "message" && event["message"]["id"] == id {
            let done = event["message"]["status"] == "sent";
            events.push(event);
            if done {
                return Ok(events);
            }
        }
    }
}
pub async fn closed(socket: &mut Ws) -> Result<()> {
    tokio::time::timeout(WAIT, async {
        loop {
            match socket.next().await {
                None | Some(Err(_)) | Some(Ok(Message::Close(_))) => return Ok(()),
                Some(Ok(Message::Ping(bytes))) => socket.send(Message::Pong(bytes)).await?,
                _ => {}
            }
        }
    })
    .await?
}
pub async fn eventually<T, F, Fut>(mut check: F) -> Result<T>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = Result<Option<T>>>,
{
    let deadline = Instant::now() + WAIT;
    loop {
        if let Some(value) = check().await? {
            return Ok(value);
        }
        ensure!(Instant::now() < deadline, "fixture condition timed out");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

pub struct Relay {
    pub dir: tempfile::TempDir,
    pub api: Api,
    pub child: Option<Child>,
    mode: String,
    pub config: PathBuf,
    tls: bool,
}
impl Relay {
    pub async fn new(mode: &str) -> Result<Self> {
        Self::new_tls(mode, false).await
    }
    pub async fn new_tls(mode: &str, tls: bool) -> Result<Self> {
        let dir = tempfile::tempdir()?;
        let port = TcpListener::bind("127.0.0.1:0")?.local_addr()?.port();
        let token = format!("fixture-{}", uuid::Uuid::new_v4());
        let config = dir.path().join("server.json");
        let mut data = json!({"listen":format!("127.0.0.1:{port}"),"token":token,"data":dir.path().join("state")});
        if mode != "demo" {
            data["bridge_program"] = json!(std::env::current_exe()?);
            data["bridge_args"] = json!(["--adapter", mode, dir.path().to_string_lossy()]);
        }
        let mut builder = Client::builder().no_proxy().timeout(WAIT);
        let mut tls_config = None;
        if tls {
            let mut make = child_command(binary());
            make.arg("--make-cert").current_dir(dir.path());
            ensure!(
                run_output(make).await?.status.success(),
                "fixture TLS generation failed"
            );
            let cert = dir.path().join(".state/tls/cert.pem");
            data["tls_cert"] = json!(cert);
            data["tls_key"] = json!(cert.with_file_name("key.pem"));
            let mut roots = RootCertStore::empty();
            roots.add(CertificateDer::from_pem_slice(&std::fs::read(cert)?)?)?;
            let config = ClientConfig::builder_with_provider(Arc::new(
                rustls::crypto::ring::default_provider(),
            ))
            .with_safe_default_protocol_versions()?
            .with_root_certificates(roots)
            .with_no_client_auth();
            builder = builder.use_preconfigured_tls(config.clone());
            tls_config = Some(Arc::new(config));
        }
        std::fs::write(&config, serde_json::to_vec(&data)?)?;
        let api = Api {
            base: format!("{}://127.0.0.1:{port}", if tls { "https" } else { "http" }),
            token,
            client: builder.build()?,
            tls: tls_config,
        };
        let mut relay = Self {
            dir,
            api,
            child: None,
            mode: mode.into(),
            config,
            tls,
        };
        relay.start().await?;
        Ok(relay)
    }
    pub async fn start(&mut self) -> Result<()> {
        ensure!(self.child.is_none(), "fixture already started");
        let mut command = child_command(binary());
        command.arg("--config").arg(&self.config);
        if self.mode == "demo" {
            command.arg("--demo");
        }
        command
            .stdout(std::fs::File::create(
                self.dir.path().join("server-stdout.log"),
            )?)
            .stderr(std::fs::File::create(
                self.dir.path().join("server-stderr.log"),
            )?);
        self.child = Some(command.spawn()?);
        let deadline = Instant::now() + WAIT;
        loop {
            if self.child.as_mut().unwrap().try_wait()?.is_some() {
                bail!(
                    "fixture relay exited: {}",
                    std::fs::read_to_string(self.dir.path().join("server-stderr.log"))?
                );
            }
            if let Ok(value) = self.api.get("/api/status").await
                && value["ready"] == true
            {
                break;
            }
            ensure!(
                Instant::now() < deadline,
                "fixture relay startup timed out (tls={})",
                self.tls
            );
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        Ok(())
    }
    pub async fn stop(&mut self) -> Result<()> {
        self.api.post("/api/shutdown", json!({})).await?;
        self.wait_exit().await
    }
    pub async fn wait_exit(&mut self) -> Result<()> {
        let deadline = Instant::now() + WAIT;
        loop {
            if let Some(status) = self
                .child
                .as_mut()
                .context("fixture process missing")?
                .try_wait()?
            {
                ensure!(
                    status.success(),
                    "fixture relay failed: {}",
                    std::fs::read_to_string(self.dir.path().join("server-stderr.log"))?
                );
                self.child = None;
                return Ok(());
            }
            ensure!(Instant::now() < deadline, "fixture relay exit timed out");
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }
    pub fn stored(&self, id: &str) -> Result<Option<Value>> {
        let connection = Connection::open_with_flags(
            self.dir.path().join("state/relay.sqlite"),
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
        )?;
        let body: Option<String> = connection
            .query_row("SELECT body FROM messages WHERE id=?1", [id], |r| r.get(0))
            .optional()?;
        Ok(body.map(|s| serde_json::from_str(&s)).transpose()?)
    }
    pub fn change(&self, id: &str, fields: Value) -> Result<()> {
        let mut value = self.stored(id)?.context("fixture row not found")?;
        value
            .as_object_mut()
            .unwrap()
            .extend(fields.as_object().unwrap().clone());
        Connection::open(self.dir.path().join("state/relay.sqlite"))?.execute(
            "UPDATE messages SET body=?1,status=?2 WHERE id=?3",
            params![value.to_string(), value["status"].as_str().unwrap(), id],
        )?;
        Ok(())
    }
    pub async fn state(&self, id: &str, status: &str) -> Result<Value> {
        eventually(|| async { Ok(self.stored(id)?.filter(|v| v["status"] == status)) }).await
    }
    pub async fn confirmed(&self, text: &str, fields: Value) -> Result<Value> {
        let mut value = message("fixture-a", text);
        value
            .as_object_mut()
            .unwrap()
            .extend(fields.as_object().unwrap().clone());
        self.api.post("/api/messages", value.clone()).await?;
        self.state(value["id"].as_str().unwrap(), "sent").await
    }
    pub fn commands(&self, op: &str) -> Result<Vec<Value>> {
        let path = self.dir.path().join("commands.jsonl");
        if !path.exists() {
            return Ok(Vec::new());
        }
        std::fs::read(path)?
            .split_inclusive(|byte| *byte == b'\n')
            .filter(|line| line.ends_with(b"\n"))
            .map(serde_json::from_slice::<Value>)
            .collect::<std::result::Result<Vec<_>, _>>()
            .map(|rows| rows.into_iter().filter(|r| r["op"] == op).collect())
            .map_err(Into::into)
    }
    pub async fn rows(&self, group: &str) -> Result<Value> {
        self.api
            .get(&query("/api/messages", &[("group", group.into())]))
            .await
    }
    pub async fn nonempty(&self, group: &str) -> Result<Value> {
        eventually(|| async {
            let rows = self.rows(group).await?;
            Ok((!rows.as_array().unwrap().is_empty()).then_some(rows))
        })
        .await
    }
}
impl Drop for Relay {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            if child.try_wait().ok().flatten().is_none() {
                let api = self.api.clone();
                if let Ok(handle) = tokio::runtime::Handle::try_current() {
                    tokio::task::block_in_place(|| {
                        let _ = handle.block_on(async {
                            tokio::time::timeout(
                                Duration::from_secs(1),
                                api.post("/api/shutdown", json!({})),
                            )
                            .await
                        });
                    });
                }
                let deadline = Instant::now() + Duration::from_secs(5);
                while child.try_wait().ok().flatten().is_none() && Instant::now() < deadline {
                    std::thread::sleep(Duration::from_millis(10));
                }
                let _ = child.kill();
            }
            let _ = child.wait();
        }
    }
}
