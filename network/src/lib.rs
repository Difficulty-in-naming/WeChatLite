//! An identity-pinned QUIC tunnel to one fixed, loopback TLS listener.

use anyhow::{Context, Result, bail, ensure};
use futures_util::StreamExt;
pub use iroh::Endpoint;
use iroh::{
    EndpointAddr, EndpointId, RelayConfig, RelayMap, RelayMode, RelayUrl, SecretKey,
    endpoint::{Connection, QuicTransportConfig, presets},
};
use iroh_relay::{RelayQuicConfig, tls::CaTlsConfig};
use serde::{Deserialize, Serialize};
use std::{
    fmt,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    net::SocketAddr,
    path::Path,
    sync::Arc,
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::{TcpListener, TcpStream},
    sync::{Mutex, Semaphore, mpsc, watch},
    task::JoinSet,
    time::timeout,
};

pub const ALPN: &[u8] = b"wechat-lite/http-tunnel/1";
pub const COPY_BUFFER_BYTES: usize = 64 * 1024;
const MAX_CONNECTIONS: usize = 32;
const MAX_STREAMS: usize = 128;
const STREAMS_PER_CONNECTION: u32 = 16;
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(15);
const TCP_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub enabled: bool,
    pub relay_url: String,
    pub relay_token: String,
    pub relay_ca: String,
    pub relay_quic_port: u16,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            enabled: false,
            relay_url: String::new(),
            relay_token: String::new(),
            relay_ca: String::new(),
            relay_quic_port: 443,
        }
    }
}

impl fmt::Debug for Settings {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Settings")
            .field("enabled", &self.enabled)
            .field("relay_url", &self.relay_url)
            .field("relay_token", &"<redacted>")
            .field("relay_ca_bytes", &self.relay_ca.len())
            .field("relay_quic_port", &self.relay_quic_port)
            .finish()
    }
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        ensure!(self.relay_url.len() <= 2048, "relay URL is too long");
        ensure!(self.relay_token.len() <= 512, "relay token is too long");
        ensure!(self.relay_ca.len() <= 16 * 1024, "relay CA is too large");
        if !self.enabled {
            return Ok(());
        }
        self.relay()?;
        ensure!(self.relay_quic_port != 0, "relay QUIC port must be nonzero");
        ensure!(
            !self.relay_token.is_empty()
                && self.relay_token.bytes().all(|b| (33..=126).contains(&b)),
            "relay token must contain visible ASCII without spaces"
        );
        self.tls_config()?;
        Ok(())
    }

    fn relay(&self) -> Result<RelayUrl> {
        let url = url::Url::parse(&self.relay_url).context("invalid relay URL")?;
        let local = url.host_str().is_some_and(|host| {
            host == "localhost"
                || host
                    .trim_matches(['[', ']'])
                    .parse::<std::net::IpAddr>()
                    .is_ok_and(|ip| ip.is_loopback())
        });
        ensure!(
            url.scheme() == "https" || (url.scheme() == "http" && local),
            "relay requires HTTPS (HTTP is only allowed on loopback)"
        );
        ensure!(
            url.host().is_some() && url.port() != Some(0),
            "invalid relay host or port"
        );
        ensure!(
            url.username().is_empty()
                && url.password().is_none()
                && url.query().is_none()
                && url.fragment().is_none()
                && url.path() == "/"
                && self.relay_url.trim() == self.relay_url,
            "relay URL cannot contain credentials, query, fragment or a path"
        );
        self.relay_url
            .parse()
            .map_err(|_| anyhow::anyhow!("invalid relay URL"))
    }

    fn tls_config(&self) -> Result<CaTlsConfig> {
        if self.relay_ca.trim().is_empty() {
            return Ok(CaTlsConfig::embedded());
        }
        let mut roots = Vec::new();
        let mut store = rustls::RootCertStore::empty();
        for item in rustls_pemfile::read_all(&mut self.relay_ca.as_bytes()) {
            match item.context("invalid relay CA PEM")? {
                rustls_pemfile::Item::X509Certificate(cert) => {
                    ensure!(roots.len() < 8, "too many relay CA certificates");
                    store
                        .add(cert.clone())
                        .context("invalid relay CA certificate")?;
                    roots.push(cert);
                }
                _ => bail!("relay CA must contain only public certificates"),
            }
        }
        ensure!(!roots.is_empty(), "relay CA contains no certificates");
        Ok(CaTlsConfig::custom_roots(roots))
    }
}

fn transport() -> QuicTransportConfig {
    QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(STREAMS_PER_CONNECTION.into())
        .max_concurrent_uni_streams(0u32.into())
        .datagram_receive_buffer_size(None)
        .datagram_send_buffer_size(0)
        .stream_receive_window((256u32 * 1024).into())
        .receive_window((512u32 * 1024).into())
        .send_window(512 * 1024)
        .max_idle_timeout(Some(
            Duration::from_secs(60)
                .try_into()
                .expect("valid idle timeout"),
        ))
        .keep_alive_interval(Duration::from_secs(15))
        .build()
}

/// Binds only the configured private relay. No pkarr or public address discovery is enabled.
pub async fn bind(settings: &Settings, key_path: &Path) -> Result<Endpoint> {
    bind_client(settings, key_path, false).await
}

/// The helper's explicit relay-only diagnostic mode never changes server Settings.
pub async fn bind_client(
    settings: &Settings,
    key_path: &Path,
    relay_only: bool,
) -> Result<Endpoint> {
    settings.validate()?;
    ensure!(settings.enabled, "P2P is disabled");
    let key = identity_key(key_path)?;
    let relay = RelayConfig::new(
        settings.relay()?,
        Some(RelayQuicConfig::new(settings.relay_quic_port)),
    )
    .with_auth_token(settings.relay_token.clone());
    let map: RelayMap = [relay].into_iter().collect();
    let builder = Endpoint::builder(presets::Minimal)
        .secret_key(key)
        .alpns(vec![ALPN.to_vec()])
        .relay_mode(RelayMode::Custom(map))
        .ca_tls_config(settings.tls_config()?)
        .transport_config(transport());
    let builder = if relay_only {
        builder.clear_ip_transports()
    } else {
        builder
    };
    builder
        .bind()
        .await
        .map_err(|_| anyhow::anyhow!("failed to bind private P2P endpoint"))
}

fn open_key(path: &Path) -> io::Result<File> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.custom_flags(windows_sys::Win32::Storage::FileSystem::FILE_FLAG_OPEN_REPARSE_POINT);
    }
    options.open(path)
}

#[cfg(unix)]
fn create_key(path: &Path) -> io::Result<File> {
    use std::os::unix::fs::OpenOptionsExt;
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)
}

#[cfg(windows)]
fn create_key(path: &Path) -> io::Result<File> {
    use std::{
        os::windows::{ffi::OsStrExt, io::FromRawHandle},
        ptr,
    };
    use windows_sys::Win32::{
        Foundation::{GENERIC_WRITE, INVALID_HANDLE_VALUE, LocalFree},
        Security::{
            Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW,
            SECURITY_ATTRIBUTES,
        },
        Storage::FileSystem::{
            CREATE_NEW, CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OPEN_REPARSE_POINT,
        },
    };
    let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    // Protected DACL: only the object owner and LocalSystem, no inherited group/world access.
    let sddl: Vec<u16> = "D:P(A;;FA;;;OW)(A;;FA;;;SY)"
        .encode_utf16()
        .chain(Some(0))
        .collect();
    let mut descriptor = ptr::null_mut();
    unsafe {
        if ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            1,
            &mut descriptor,
            ptr::null_mut(),
        ) == 0
        {
            return Err(io::Error::last_os_error());
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor,
            bInheritHandle: 0,
        };
        let handle = CreateFileW(
            name.as_ptr(),
            GENERIC_WRITE,
            0,
            &attributes,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            ptr::null_mut(),
        );
        let result = if handle == INVALID_HANDLE_VALUE {
            Err(io::Error::last_os_error())
        } else {
            Ok(File::from_raw_handle(handle))
        };
        LocalFree(descriptor);
        result
    }
}

fn read_key(mut file: File) -> Result<SecretKey> {
    let metadata = file.metadata().context("cannot inspect P2P identity key")?;
    ensure!(
        metadata.is_file() && metadata.len() == 32,
        "P2P identity key must be exactly 32 bytes"
    );
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        ensure!(
            metadata.permissions().mode() & 0o077 == 0,
            "P2P identity key permissions must be 0600"
        );
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        ensure!(
            metadata.file_attributes()
                & windows_sys::Win32::Storage::FileSystem::FILE_ATTRIBUTE_REPARSE_POINT
                == 0,
            "P2P identity key cannot be a reparse point"
        );
    }
    let mut bytes = [0u8; 32];
    file.read_exact(&mut bytes)
        .context("cannot read P2P identity key")?;
    let mut extra = [0];
    ensure!(
        file.read(&mut extra)? == 0,
        "P2P identity key changed while reading"
    );
    let key = SecretKey::from_bytes(&bytes);
    bytes.fill(0);
    Ok(key)
}

fn identity_key(path: &Path) -> Result<SecretKey> {
    ensure!(path.is_absolute(), "P2P key_file must be absolute");
    match open_key(path) {
        Ok(file) => return read_key(file),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(_) => bail!("cannot open existing P2P identity key"),
    }
    let parent = path
        .parent()
        .context("P2P identity key needs a parent directory")?;
    fs::create_dir_all(parent).context("cannot create P2P identity directory")?;
    let key = SecretKey::generate();
    match create_key(path) {
        Ok(mut file) => {
            let mut bytes = key.to_bytes();
            file.write_all(&bytes)
                .context("cannot persist P2P identity key")?;
            file.sync_all().context("cannot sync P2P identity key")?;
            bytes.fill(0);
            Ok(key)
        }
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
            read_key(open_key(path).context("cannot read concurrently created P2P identity key")?)
        }
        Err(_) => bail!("cannot create P2P identity key"),
    }
}

fn validate_target(target: SocketAddr) -> Result<()> {
    ensure!(
        target.ip().is_loopback() && target.port() != 0,
        "P2P target must be a fixed loopback port"
    );
    Ok(())
}

async fn stopped(shutdown: &mut watch::Receiver<bool>) {
    loop {
        if *shutdown.borrow_and_update() {
            return;
        }
        if shutdown.changed().await.is_err() {
            return;
        }
    }
}

/// Each QUIC bidirectional stream carries one unmodified TLS/TCP connection.
/// The target is fixed by the local server, never supplied by a remote peer.
pub async fn serve(
    endpoint: Endpoint,
    target: SocketAddr,
    mut shutdown: watch::Receiver<bool>,
) -> Result<()> {
    validate_target(target)?;
    let connections = Arc::new(Semaphore::new(MAX_CONNECTIONS));
    let streams = Arc::new(Semaphore::new(MAX_STREAMS));
    let mut tasks = JoinSet::new();
    loop {
        while tasks.try_join_next().is_some() {}
        tokio::select! {
            biased;
            _ = stopped(&mut shutdown) => break,
            incoming = endpoint.accept() => {
                let Some(incoming) = incoming else { break; };
                let Ok(permit) = connections.clone().try_acquire_owned() else {
                    incoming.refuse();
                    continue;
                };
                let streams = streams.clone();
                tasks.spawn(async move {
                    let _permit = permit;
                    if let Ok(Ok(connection)) = timeout(HANDSHAKE_TIMEOUT, incoming).await {
                        if connection.alpn() == ALPN {
                            serve_connection(connection, target, streams).await;
                        } else {
                            connection.close(1u32.into(), b"unsupported protocol");
                        }
                    }
                });
            },
            _ = tasks.join_next(), if !tasks.is_empty() => {},
        }
    }
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    let _ = timeout(Duration::from_secs(3), endpoint.close()).await;
    Ok(())
}

async fn serve_connection(connection: Connection, target: SocketAddr, permits: Arc<Semaphore>) {
    let mut tasks = JoinSet::new();
    loop {
        while tasks.try_join_next().is_some() {}
        tokio::select! {
            result = connection.accept_bi() => {
                let Ok((mut send, mut recv)) = result else { break; };
                let Ok(permit) = permits.clone().try_acquire_owned() else {
                    let _ = send.reset(1u32.into());
                    let _ = recv.stop(1u32.into());
                    continue;
                };
                tasks.spawn(async move {
                    let _permit = permit;
                    let stream = tokio::io::join(recv, send);
                    let _ = forward_to_target(stream, target).await;
                });
            },
            _ = tasks.join_next(), if !tasks.is_empty() => {},
        }
    }
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    connection.close(0u32.into(), b"closed");
}

async fn forward_to_target<S>(mut stream: S, target: SocketAddr) -> Result<()>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let prefix = tls_prefix(&mut stream).await?;
    let mut tcp = timeout(TCP_TIMEOUT, TcpStream::connect(target))
        .await
        .context("loopback TLS connection timed out")??;
    tcp.set_nodelay(true)?;
    timeout(TCP_TIMEOUT, tcp.write_all(&prefix))
        .await
        .context("loopback TLS write timed out")??;
    // There is deliberately no retry here: a POST/WS frame already copied is never replayed.
    copy_streams(&mut stream, &mut tcp).await?;
    Ok(())
}

async fn tls_prefix<S: AsyncRead + Unpin>(stream: &mut S) -> Result<[u8; 6]> {
    let mut prefix = [0u8; 6];
    timeout(HANDSHAKE_TIMEOUT, stream.read_exact(&mut prefix))
        .await
        .context("TLS ClientHello timed out")??;
    let record_length = u16::from_be_bytes([prefix[3], prefix[4]]);
    ensure!(
        prefix[0] == 22
            && prefix[1] == 3
            && (1..=3).contains(&prefix[2])
            && (1..=18_432).contains(&record_length)
            && prefix[5] == 1,
        "tunnel only accepts TLS ClientHello"
    );
    Ok(prefix)
}

async fn copy_streams<A, B>(a: &mut A, b: &mut B) -> io::Result<(u64, u64)>
where
    A: AsyncRead + AsyncWrite + Unpin,
    B: AsyncRead + AsyncWrite + Unpin,
{
    tokio::io::copy_bidirectional_with_sizes(a, b, COPY_BUFFER_BYTES, COPY_BUFFER_BYTES).await
}

/// Helper-process events contain no application keys, relay tokens, paths or request bytes.
#[derive(Clone, Serialize, Debug)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum Event {
    Ready { url: String },
    Path { path: &'static str },
    Error { message: &'static str },
}

struct Client {
    endpoint: Endpoint,
    remote: EndpointAddr,
    connection: Mutex<Option<Connection>>,
    events: mpsc::Sender<Event>,
}

impl Client {
    async fn connection(&self) -> Result<Connection> {
        let mut cached = self.connection.lock().await;
        if let Some(connection) = cached
            .as_ref()
            .filter(|connection| connection.close_reason().is_none())
        {
            return Ok(connection.clone());
        }
        let connection = timeout(
            HANDSHAKE_TIMEOUT,
            self.endpoint.connect(self.remote.clone(), ALPN),
        )
        .await
        .context("P2P connection timed out")?
        .map_err(|_| anyhow::anyhow!("P2P connection failed"))?;
        ensure!(
            connection.remote_id() == self.remote.id,
            "remote P2P identity mismatch"
        );
        tokio::spawn(watch_path(connection.clone(), self.events.clone()));
        *cached = Some(connection.clone());
        Ok(connection)
    }

    async fn tunnel(&self, mut tcp: TcpStream) -> Result<()> {
        tcp.set_nodelay(true)?;
        let prefix = tls_prefix(&mut tcp).await?;
        let connection = timeout(HANDSHAKE_TIMEOUT, self.connection())
            .await
            .context("P2P connection queue timed out")??;
        let (send, recv) = timeout(HANDSHAKE_TIMEOUT, connection.open_bi())
            .await
            .context("P2P stream timed out")?
            .map_err(|_| anyhow::anyhow!("P2P stream failed"))?;
        let mut stream = tokio::io::join(recv, send);
        timeout(HANDSHAKE_TIMEOUT, stream.write_all(&prefix))
            .await
            .context("P2P ClientHello write timed out")??;
        copy_streams(&mut tcp, &mut stream).await?;
        Ok(())
    }
}

async fn watch_path(connection: Connection, events: mpsc::Sender<Event>) {
    let mut changes = connection.path_events();
    let mut previous = None;
    loop {
        let selected = connection
            .paths()
            .iter()
            .find(|path| path.is_selected())
            .map(|path| if path.is_ip() { "direct" } else { "relay" });
        if let Some(path) = selected.filter(|path| Some(*path) != previous) {
            let _ = events.try_send(Event::Path { path });
            previous = Some(path);
        }
        if changes.next().await.is_none() {
            break;
        }
    }
}

/// Runs the helper's local ingress. It connects only the authenticated endpoint ID plus private relay.
pub async fn client(
    endpoint: Endpoint,
    server_id: EndpointId,
    relay_url: RelayUrl,
    shutdown: watch::Receiver<bool>,
    events: mpsc::Sender<Event>,
) -> Result<()> {
    client_with_address(
        endpoint,
        EndpointAddr::new(server_id).with_relay_url(relay_url),
        shutdown,
        events,
    )
    .await
}

async fn client_with_address(
    endpoint: Endpoint,
    remote: EndpointAddr,
    mut shutdown: watch::Receiver<bool>,
    events: mpsc::Sender<Event>,
) -> Result<()> {
    let client = Arc::new(Client {
        endpoint: endpoint.clone(),
        remote,
        connection: Mutex::new(None),
        events: events.clone(),
    });
    tokio::select! {
        biased;
        _ = stopped(&mut shutdown) => { endpoint.close().await; return Ok(()); },
        result = client.connection() => { result?; },
    }
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    events
        .send(Event::Ready {
            url: format!("https://127.0.0.1:{}", listener.local_addr()?.port()),
        })
        .await
        .context("helper output is closed")?;
    let permits = Arc::new(Semaphore::new(64));
    let mut tasks = JoinSet::new();
    loop {
        while tasks.try_join_next().is_some() {}
        tokio::select! {
            biased;
            _ = stopped(&mut shutdown) => break,
            accepted = listener.accept() => {
                let (tcp, _) = accepted?;
                let Ok(permit) = permits.clone().try_acquire_owned() else { drop(tcp); continue; };
                let client = client.clone();
                tasks.spawn(async move {
                    let _permit = permit;
                    // A cancelled HTTP/TLS/WS stream is local to that socket. It must not emit
                    // a fatal helper error or tear down unrelated streams/listener.
                    let _ = client.tunnel(tcp).await;
                });
            },
            _ = tasks.join_next(), if !tasks.is_empty() => {},
        }
    }
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    let _ = timeout(Duration::from_secs(3), endpoint.close()).await;
    Ok(())
}

/// JNI-facing exports used by the Android client. Desktop builds keep using `main.rs`.
#[cfg(target_os = "android")]
pub mod jni;

#[cfg(test)]
mod tests;
