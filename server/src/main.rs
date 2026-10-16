mod access;
mod announcement;
mod bridge;
mod history;
mod model;
mod poster;
mod records;
mod store;
mod web;

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::Arc,
};
use tokio::sync::{RwLock, Semaphore, broadcast, mpsc, watch};

#[derive(Clone, Serialize, Deserialize)]
struct Config {
    listen: String,
    token: String,
    #[serde(default = "default_data")]
    data: PathBuf,
    #[serde(default)]
    bridge_program: String,
    #[serde(default)]
    bridge_args: Vec<String>,
    #[serde(default)]
    tls_cert: String,
    #[serde(default)]
    tls_key: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    p2p: Option<wechat_p2p::Settings>,
}
fn default_data() -> PathBuf {
    PathBuf::from(".state")
}

fn lock_data(directory: &Path) -> Result<std::fs::File> {
    let directory = std::fs::canonicalize(directory).context("无法解析服务数据目录")?;
    let lock = std::fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(directory.join(".relay.lock"))
        .context("无法打开服务数据目录锁")?;
    lock.try_lock()
        .context("数据目录已被另一服务占用或无法取得独占锁，请先停止使用此目录的服务")?;
    Ok(lock)
}

struct App {
    config: Config,
    store: Arc<store::Store>,
    events: broadcast::Sender<Value>,
    commands: mpsc::Sender<Value>,
    status: RwLock<Value>,
    demo: bool,
    transfers: Arc<Semaphore>,
    posters: Semaphore,
    shutdown: watch::Sender<bool>,
    forwarding: RwLock<BTreeSet<String>>,
    forwarding_changed: watch::Sender<()>,
    clients: RwLock<access::Clients>,
    clients_changed: watch::Sender<()>,
    online_clients: std::sync::Mutex<std::collections::BTreeMap<String, usize>>,
    history: history::Requests,
    announcements: announcement::Requests,
    p2p_status: RwLock<Value>,
}

enum PreparedListener {
    Plain(tokio::net::TcpListener),
    Tls(axum_server::Server<SocketAddr, axum_server::tls_rustls::RustlsAcceptor>),
}

struct PreparedP2p {
    endpoint: wechat_p2p::Endpoint,
    listener: PreparedListener,
    target: SocketAddr,
}

async fn prepare_p2p(
    config: &Config,
    tls: Option<&axum_server::tls_rustls::RustlsConfig>,
) -> Result<Option<PreparedP2p>> {
    let Some(settings) = config.p2p.as_ref().filter(|settings| settings.enabled) else {
        return Ok(None);
    };
    let tls = tls.context("启用 P2P 需要同时配置 TLS 证书和私钥")?;
    let listener =
        std::net::TcpListener::bind("127.0.0.1:0").context("无法监听 P2P 客户端专用地址")?;
    listener.set_nonblocking(true)?;
    let target = listener.local_addr()?;
    let listener = PreparedListener::Tls(axum_server::from_tcp_rustls(listener, tls.clone())?);
    let endpoint = wechat_p2p::bind(settings, &config.data.join("p2p.key"))
        .await
        .context("无法初始化 P2P 连接服务")?;
    Ok(Some(PreparedP2p {
        endpoint,
        listener,
        target,
    }))
}

async fn serve_listener(
    listener: PreparedListener,
    router: axum::Router,
    shutdown: watch::Receiver<bool>,
) -> Result<()> {
    match listener {
        PreparedListener::Plain(listener) => {
            let server = axum::serve(
                listener,
                router.into_make_service_with_connect_info::<SocketAddr>(),
            )
            .with_graceful_shutdown(wait_for_shutdown(shutdown.clone()));
            tokio::select! {
                result = server => result.map_err(anyhow::Error::from),
                _ = async {
                    wait_for_shutdown(shutdown).await;
                    tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                } => Ok(()),
            }
        }
        PreparedListener::Tls(server) => {
            let handle = axum_server::Handle::new();
            let stop = handle.clone();
            let watcher = tokio::spawn(async move {
                wait_for_shutdown(shutdown).await;
                stop.graceful_shutdown(Some(std::time::Duration::from_secs(5)));
            });
            let result = server
                .handle(handle)
                .serve(router.into_make_service_with_connect_info::<SocketAddr>())
                .await
                .map_err(anyhow::Error::from);
            watcher.abort();
            result
        }
    }
}

async fn run_p2p(app: Arc<App>, prepared: PreparedP2p) {
    let (stop, stopped) = watch::channel(false);
    let mut tasks = tokio::task::JoinSet::new();
    tasks.spawn(serve_listener(
        prepared.listener,
        web::client_router(app.clone()),
        stopped.clone(),
    ));
    tasks.spawn(wechat_p2p::serve(
        prepared.endpoint.clone(),
        prepared.target,
        stopped,
    ));
    let failed = tokio::select! {
        _ = wait_for_shutdown(app.shutdown.subscribe()) => false,
        _ = tasks.join_next() => !*app.shutdown.borrow(),
    };
    app.p2p_status.write().await["status"] = if failed {
        "failed".into()
    } else {
        "stopped".into()
    };
    stop.send_replace(true);
    prepared.endpoint.close().await;
    let _ = tokio::time::timeout(std::time::Duration::from_secs(6), async {
        while tasks.join_next().await.is_some() {}
    })
    .await;
    tasks.shutdown().await;
    if failed {
        eprintln!("P2P 连接服务已停止，请检查网络设置；本机服务仍可使用。");
    }
}

async fn wait_for_shutdown(mut receiver: watch::Receiver<bool>) {
    while !*receiver.borrow_and_update() {
        if receiver.changed().await.is_err() {
            break;
        }
    }
}

#[cfg(test)]
fn test_app(directory: &Path) -> (Arc<App>, mpsc::Receiver<Value>) {
    let mut config = default_config();
    config.data = directory.to_owned();
    std::fs::create_dir_all(config.data.join("media")).unwrap();
    let store = Arc::new(store::Store::open(&directory.join("test.sqlite")).unwrap());
    store
        .set_groups(&[model::Group {
            id: "g".into(),
            name: "Fixture".into(),
            count: 1,
            avatar: String::new(),
        }])
        .unwrap();
    store
        .initialize_forward_groups(&BTreeSet::from(["g".into()]))
        .unwrap();
    let forwarding = store.forward_groups().unwrap().unwrap_or_default();
    let (events, _) = broadcast::channel(256);
    let (commands, receiver) = mpsc::channel(100);
    let (shutdown, _) = watch::channel(false);
    let (forwarding_changed, _) = watch::channel(());
    let (clients_changed, _) = watch::channel(());
    (
        Arc::new(App {
            config,
            store,
            events,
            commands,
            status: RwLock::new(json!({"type":"status","ready":false})),
            demo: false,
            transfers: Arc::new(Semaphore::new(3)),
            posters: Semaphore::new(1),
            shutdown,
            forwarding: RwLock::new(forwarding),
            forwarding_changed,
            clients: RwLock::new(Default::default()),
            clients_changed,
            online_clients: Default::default(),
            history: Default::default(),
            announcements: Default::default(),
            p2p_status: RwLock::new(json!({"enabled":false,"server_id":"","status":"disabled"})),
        }),
        receiver,
    )
}
impl App {
    async fn allows(&self, group: &str) -> bool {
        self.forwarding.read().await.contains(group)
    }
    async fn set_forwarding(&self, groups: BTreeSet<String>) -> Result<()> {
        let mut current = self.forwarding.write().await;
        let events = self
            .db({
                let groups = groups.clone();
                move |s| s.set_forward_groups(&groups)
            })
            .await?;
        *current = groups;
        self.forwarding_changed.send_replace(());
        for event in events {
            self.publish(event);
        }
        self.publish(json!({"type":"groups"}));
        Ok(())
    }
    async fn db<T: Send + 'static>(
        &self,
        f: impl FnOnce(&store::Store) -> Result<T> + Send + 'static,
    ) -> Result<T> {
        let store = self.store.clone();
        tokio::task::spawn_blocking(move || f(&store)).await?
    }
    fn publish(&self, value: Value) {
        let _ = self.events.send(web::public(value));
    }
    async fn set_status(&self, ready: bool, text: &str) {
        let previous = self.status.read().await.clone();
        self.set_status_recall(
            ready,
            text,
            "",
            previous["recall_supported"].as_bool(),
            previous["recall_reason"].as_str().unwrap_or(""),
        )
        .await;
    }
    async fn set_status_identity(&self, ready: bool, text: &str, identity: &str) {
        self.set_status_recall(ready, text, identity, None, "")
            .await;
    }
    async fn set_status_recall(
        &self,
        ready: bool,
        text: &str,
        identity: &str,
        recall_supported: Option<bool>,
        recall_reason: &str,
    ) {
        let identity = if ready
            && identity.len() <= 1024
            && identity.trim() == identity
            && !identity.chars().any(char::is_control)
            && identity != "notify@all"
        {
            identity
        } else {
            ""
        };
        let mut status = json!({"type":"status", "ready":ready, "text":text, "demo":self.demo,"self_id":identity});
        if let Some(supported) = if self.demo {
            Some(true)
        } else {
            recall_supported
        } {
            status["recall_supported"] = supported.into();
            let reason = if supported {
                ""
            } else if !recall_reason.trim().is_empty()
                && recall_reason.chars().count() <= 256
                && !recall_reason.chars().any(char::is_control)
            {
                recall_reason
            } else {
                model::RECALL_UNAVAILABLE
            };
            status["recall_reason"] = reason.into();
        }
        *self.status.write().await = status.clone();
        self.publish(status);
    }
}

fn default_config() -> Config {
    Config {
        listen: "127.0.0.1:8765".into(),
        token: format!(
            "{}{}",
            uuid::Uuid::new_v4().simple(),
            uuid::Uuid::new_v4().simple()
        ),
        data: default_data(),
        bridge_program: String::new(),
        bridge_args: vec![],
        tls_cert: String::new(),
        tls_key: String::new(),
        p2p: None,
    }
}

#[tokio::main(worker_threads = 2)]
async fn main() -> Result<()> {
    // Keep TLS deterministic when Cargo unifies the relay and adapter features.
    let _ = rustls::crypto::ring::default_provider().install_default();
    let args: Vec<String> = std::env::args().collect();
    let get = |name: &str| args.windows(2).find(|a| a[0] == name).map(|a| a[1].clone());
    let config_path = PathBuf::from(get("--config").unwrap_or_else(|| "server.local.json".into()));
    if args.iter().any(|a| a == "--init") {
        if config_path.exists() {
            bail!("配置已存在，未覆盖");
        }
        std::fs::write(
            &config_path,
            serde_json::to_string_pretty(&default_config())?,
        )?;
        println!(
            "已创建 {}，请设置本机微信适配器后启动。",
            config_path.display()
        );
        return Ok(());
    }
    if args.iter().any(|a| a == "--make-cert") {
        let names = get("--names").unwrap_or_else(|| "localhost,127.0.0.1".into());
        let cert = rcgen::generate_simple_self_signed(
            names.split(',').map(str::to_owned).collect::<Vec<_>>(),
        )?;
        std::fs::create_dir_all(".state/tls")?;
        std::fs::write(".state/tls/cert.pem", cert.cert.pem())?;
        std::fs::write(".state/tls/key.pem", cert.signing_key.serialize_pem())?;
        println!("已创建 .state/tls/cert.pem 和 key.pem；证书需要在客户端显式信任。");
        return Ok(());
    }
    let demo = args.iter().any(|a| a == "--demo");
    let mut config: Config = if config_path.exists() {
        serde_json::from_slice(&std::fs::read(&config_path)?)?
    } else {
        default_config()
    };
    if let Some(value) = get("--listen") {
        config.listen = value;
    }
    if let Some(value) = get("--data") {
        config.data = value.into();
    }
    if let Some(value) = get("--token") {
        config.token = value;
    }
    if let Some(value) = get("--bridge") {
        config.bridge_program = value;
    }
    if config.token.len() < 24 {
        bail!("访问密钥至少需要 24 个字符");
    }
    let address: SocketAddr = config.listen.parse().context("listen 必须为 IP:端口")?;
    if config.tls_cert.is_empty() != config.tls_key.is_empty() {
        bail!("TLS 证书与私钥必须同时配置");
    }
    if !address.ip().is_loopback() && (config.tls_cert.is_empty() || config.tls_key.is_empty()) {
        bail!("对其他电脑开放服务需要配置 TLS 证书和私钥");
    }
    config.data = std::path::absolute(&config.data)?;
    std::fs::create_dir_all(&config.data)?;
    config.data = std::fs::canonicalize(&config.data)?;
    let _data_lock = lock_data(&config.data)?;
    let tls = if config.tls_cert.is_empty() {
        None
    } else {
        Some(
            axum_server::tls_rustls::RustlsConfig::from_pem_file(
                Path::new(&config.tls_cert),
                Path::new(&config.tls_key),
            )
            .await
            .context("TLS 证书或私钥无效")?,
        )
    };
    // Complete all fallible network setup before opening the store or starting WeChat.
    let listener = std::net::TcpListener::bind(address).context("无法监听配置的服务地址")?;
    listener.set_nonblocking(true)?;
    let address = listener.local_addr()?;
    let p2p = prepare_p2p(&config, tls.as_ref()).await?;
    let p2p_status = match &p2p {
        Some(prepared) => json!({
            "enabled":true,
            "server_id":prepared.endpoint.id().to_string(),
            "status":"listening"
        }),
        None => json!({"enabled":false,"server_id":"","status":"disabled"}),
    };
    let listener = match tls {
        Some(tls) => PreparedListener::Tls(axum_server::from_tcp_rustls(listener, tls)?),
        None => PreparedListener::Plain(tokio::net::TcpListener::from_std(listener)?),
    };
    std::fs::create_dir_all(config.data.join("media"))?;
    let store = Arc::new(store::Store::open(&config.data.join("relay.sqlite"))?);
    if !demo {
        store.initialize_forward_groups(&BTreeSet::new())?;
    }
    let forwarding = store.forward_groups()?.unwrap_or_default();
    let clients = store.clients()?;
    let (events, _) = broadcast::channel(256);
    let (commands, receiver) = mpsc::channel(100);
    let (shutdown, _) = watch::channel(false);
    let (forwarding_changed, _) = watch::channel(());
    let (clients_changed, _) = watch::channel(());
    let app = Arc::new(App {
        config: config.clone(),
        store,
        events,
        commands,
        status: RwLock::new(
            json!({"type":"status","ready":false,"text":"微信尚未连接","demo":demo}),
        ),
        demo,
        transfers: Arc::new(Semaphore::new(3)),
        posters: Semaphore::new(1),
        shutdown,
        forwarding: RwLock::new(forwarding),
        forwarding_changed,
        clients: RwLock::new(clients),
        clients_changed,
        online_clients: Default::default(),
        history: Default::default(),
        announcements: Default::default(),
        p2p_status: RwLock::new(p2p_status),
    });
    let bridge = bridge::start(app.clone(), receiver).await?;
    let p2p_task = p2p.map(|prepared| tokio::spawn(run_p2p(app.clone(), prepared)));
    let router = web::router(app.clone());
    println!(
        "微信轻聊服务已启动：{}://{}{}",
        if config.tls_cert.is_empty() {
            "http"
        } else {
            "https"
        },
        address,
        if demo {
            " （演示模式，不连接微信）"
        } else {
            ""
        }
    );
    let shutdown = app.shutdown.clone();
    tokio::spawn(async move {
        if tokio::signal::ctrl_c().await.is_ok() {
            shutdown.send_replace(true);
        }
    });
    let result = serve_listener(listener, router, app.shutdown.subscribe()).await;
    app.shutdown.send_replace(true);
    let _ = tokio::time::timeout(std::time::Duration::from_secs(5), bridge).await;
    if let Some(task) = p2p_task {
        let _ = tokio::time::timeout(std::time::Duration::from_secs(7), task).await;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn legacy_configuration_keeps_p2p_disabled_and_does_not_gain_credentials() {
        let legacy = serde_json::to_value(default_config()).unwrap();
        assert!(legacy.get("p2p").is_none());
        let restored: Config = serde_json::from_value(legacy.clone()).unwrap();
        assert!(restored.p2p.is_none());
        assert_eq!(serde_json::to_value(restored).unwrap(), legacy);
    }

    #[tokio::test]
    async fn p2p_requires_tls_before_endpoint_or_key_creation_and_disabled_mode_is_inert() {
        let directory = tempfile::tempdir().unwrap();
        let mut config = default_config();
        config.data = directory.path().to_owned();
        config.p2p = Some(
            serde_json::from_value(json!({
                "enabled":true,
                "relay_url":"https://relay.invalid",
                "relay_token":"fixture-token",
                "relay_ca":"",
                "relay_quic_port":7842
            }))
            .unwrap(),
        );
        assert!(prepare_p2p(&config, None).await.is_err());
        assert!(!directory.path().join("p2p.key").exists());
        config.p2p.as_mut().unwrap().enabled = false;
        assert!(prepare_p2p(&config, None).await.unwrap().is_none());
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 0);
    }

    #[test]
    fn data_lock_is_exclusive_nontruncating_and_released_with_its_handle() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join(".relay.lock");
        std::fs::write(&path, b"preserve existing bytes").unwrap();
        let first = lock_data(directory.path()).unwrap();
        assert!(lock_data(&directory.path().join(".")).is_err());
        drop(first);
        let next = lock_data(directory.path()).unwrap();
        drop(next);
        assert_eq!(std::fs::read(path).unwrap(), b"preserve existing bytes");
    }
}
