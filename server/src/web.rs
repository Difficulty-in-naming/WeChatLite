use crate::{
    App,
    access::{self, Capability, ClientAccess, ClientRecord, Clients, Principal},
    model::{ChatMessage, Media, Mention, Quote, clean_name, detect_kind},
};
use anyhow::{Context, Result, bail};
use axum::{
    Extension, Json, Router,
    body::Body,
    extract::{
        ConnectInfo, DefaultBodyLimit, Path, Query, State, WebSocketUpgrade,
        ws::{Message, WebSocket},
    },
    http::{HeaderMap, Request, StatusCode, header},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{collections::BTreeSet, net::SocketAddr, path::PathBuf, sync::Arc, time::Duration};
use subtle::ConstantTimeEq;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_util::io::ReaderStream;

const MAX_FILE: u64 = 512 * 1024 * 1024;

#[derive(Debug)]
pub struct Error(anyhow::Error, StatusCode);
impl<E: Into<anyhow::Error>> From<E> for Error {
    fn from(error: E) -> Self {
        Self(error.into(), StatusCode::BAD_REQUEST)
    }
}
impl IntoResponse for Error {
    fn into_response(self) -> Response {
        (self.1, Json(json!({"error":self.0.to_string()}))).into_response()
    }
}
type ApiResult<T> = std::result::Result<T, Error>;

pub fn public(mut value: Value) -> Value {
    match &mut value {
        Value::Object(obj) => {
            obj.remove("path");
            obj.remove("access_client");
            obj.remove("recall_client");
            obj.remove("recall_attempt");
            for child in obj.values_mut() {
                *child = public(child.take());
            }
        }
        Value::Array(arr) => {
            for child in arr {
                *child = public(child.take());
            }
        }
        _ => {}
    }
    value
}

pub fn router(app: Arc<App>) -> Router {
    build_router(app, EntryPoint::Standard)
}

pub fn client_router(app: Arc<App>) -> Router {
    build_router(app, EntryPoint::ClientsOnly)
}

#[derive(Clone, Copy)]
enum EntryPoint {
    Standard,
    ClientsOnly,
}

fn build_router(app: Arc<App>, entry: EntryPoint) -> Router {
    let mut api = Router::new()
        .route("/status", get(status))
        .route("/groups", get(groups))
        .route("/groups/{id}/activate", post(activate))
        .route("/groups/{id}/members", get(members))
        .route("/groups/{id}/announcement", get(announcement))
        .route("/messages", get(history).post(send))
        .route("/history", get(native_history))
        .route("/records", get(records))
        .route("/messages/{id}", get(locate))
        .route("/messages/{id}/quote-source", post(refresh_quote_source))
        .route("/messages/{id}/recall", post(recall))
        .route(
            "/media",
            post(upload).layer(DefaultBodyLimit::max(MAX_FILE as usize)),
        )
        .route("/media/{id}", get(download))
        .route("/media/{id}/poster", get(video_poster))
        .route("/events", get(events));
    if matches!(entry, EntryPoint::Standard) {
        api = api
            .route("/shutdown", post(shutdown))
            .route("/local/clients", get(local_clients).post(create_client))
            .route("/local/clients/{id}", post(update_client))
            .route("/local/clients/{id}/revoke", post(revoke_client))
            .route("/local/clients/{id}/credential", get(client_credential))
            .route("/local/clients/{id}/rotate", post(rotate_client))
            .route("/local/p2p", get(local_p2p))
            .route(
                "/local/forwarding",
                get(local_forwarding).post(set_forwarding),
            );
    }
    Router::new()
        .route(
            "/health",
            get(|| async { Json(json!({"app":"wechat-lite","version":"0.1.0"})) }),
        )
        .nest(
            "/api",
            api.layer(middleware::from_fn_with_state(
                (app.clone(), entry),
                authorize,
            )),
        )
        .layer(DefaultBodyLimit::max(64 * 1024))
        .with_state(app)
}

async fn authorize(
    State((app, entry)): State<(Arc<App>, EntryPoint)>,
    mut request: Request<Body>,
    next: Next,
) -> Response {
    let supplied = request
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .unwrap_or("");
    let address = request
        .extensions()
        .get::<ConnectInfo<SocketAddr>>()
        .map(|peer| peer.0);
    let principal = match authenticate(&app, supplied, address).await {
        Ok(principal) => principal,
        Err(error) => return error.into_response(),
    };
    // The listener owns this policy; proxy headers cannot turn a P2P peer into an administrator.
    if matches!(entry, EntryPoint::ClientsOnly) && matches!(principal, Principal::Admin) {
        return Error(
            anyhow::anyhow!("客户端入口不接受管理员密钥"),
            StatusCode::FORBIDDEN,
        )
        .into_response();
    }
    if *app.shutdown.borrow() {
        return (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(json!({"error":"服务正在退出"})),
        )
            .into_response();
    }
    request.extensions_mut().insert(principal);
    next.run(request).await
}
async fn authenticate(app: &App, token: &str, address: Option<SocketAddr>) -> ApiResult<Principal> {
    let supplied = access::token_hash(token);
    let expected = access::token_hash(&app.config.token);
    if bool::from(supplied.as_bytes().ct_eq(expected.as_bytes())) {
        if !address.is_some_and(|address| address.ip().is_loopback()) {
            return Err(Error(
                anyhow::anyhow!("管理员密钥仅限本机使用，请创建独立客户端密钥"),
                StatusCode::FORBIDDEN,
            ));
        }
        return Ok(Principal::Admin);
    }
    let legacy = {
        let clients = app.clients.read().await;
        clients.values().find_map(|record| {
            (bool::from(supplied.as_bytes().ct_eq(record.token_hash.as_bytes()))
                && record.access.enabled
                && !record.access.revoked)
                .then(|| {
                    (
                        record.access.id.clone(),
                        record.access.revision,
                        record.token.is_some(),
                    )
                })
        })
    };
    if let Some((id, revision, remembered)) = legacy {
        if remembered {
            return Ok(Principal::Client { id, revision });
        }
        // Serialize the one-time legacy upgrade with rotation/revocation and recheck after
        // acquiring the write lock. Normal authenticated requests never write to SQLite.
        let mut clients = app.clients.write().await;
        if let Some(record) = clients.get_mut(&id).filter(|record| {
            bool::from(supplied.as_bytes().ct_eq(record.token_hash.as_bytes()))
                && record.access.enabled
                && !record.access.revoked
        }) {
            if record.token.is_none() {
                let remembered = app
                    .db({
                        let id = id.clone();
                        let supplied = supplied.clone();
                        let token = token.to_owned();
                        move |store| store.remember_client_token(&id, &supplied, &token)
                    })
                    .await?;
                if !remembered {
                    return Err(Error(
                        anyhow::anyhow!("访问密钥无效或已停用"),
                        StatusCode::UNAUTHORIZED,
                    ));
                }
                record.token = Some(token.to_owned());
            }
            return Ok(Principal::Client {
                id,
                revision: record.access.revision,
            });
        }
    }
    Err(Error(
        anyhow::anyhow!("访问密钥无效或已停用"),
        StatusCode::UNAUTHORIZED,
    ))
}
async fn shutdown(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    app.shutdown.send_replace(true);
    Ok(Json(json!({"ok":true})))
}

async fn local_p2p(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    let status = app.p2p_status.read().await;
    Ok(Json(json!({
        "enabled":status["enabled"].as_bool().unwrap_or(false),
        "server_id":status["server_id"].as_str().unwrap_or(""),
        "status":status["status"].as_str().unwrap_or("disabled")
    })))
}
async fn principal_status(app: &App, principal: &Principal) -> Value {
    let mut status = app.status.read().await.clone();
    let selected = app.forwarding.read().await;
    let clients = app.clients.read().await;
    match principal {
        Principal::Admin => {
            status["authenticated_client"] = json!({"id":"","name":"本机管理员","admin":true});
            status["permissions"] = json!({"groups":*selected,"can_send":true,"can_download":true});
            status["online_clients"] = json!(app.online_clients.lock().unwrap().len());
        }
        Principal::Client { id, .. } => {
            if let Some(record) = clients.get(id) {
                status["authenticated_client"] =
                    json!({"id":id,"name":record.access.name,"admin":false});
                status["permissions"] = json!({"groups":record.access.groups.union(&record.matched_groups).filter(|group|selected.contains(*group)).collect::<Vec<_>>(),"can_send":record.access.can_send,"can_download":record.access.can_download});
            }
        }
    }
    status
}
async fn status(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
) -> Json<Value> {
    Json(principal_status(&app, &principal).await)
}
fn check_admin(principal: &Principal, address: SocketAddr) -> ApiResult<()> {
    if !matches!(principal, Principal::Admin) || !address.ip().is_loopback() {
        return Err(Error(
            anyhow::anyhow!("仅本机管理员可以执行此操作"),
            StatusCode::FORBIDDEN,
        ));
    }
    Ok(())
}
fn check_access(
    clients: &Clients,
    principal: &Principal,
    group: &str,
    capability: Capability,
) -> ApiResult<()> {
    if !principal.allows(clients, group, capability) {
        return Err(Error(
            anyhow::anyhow!("客户端未获准执行此操作，或权限已改变"),
            StatusCode::FORBIDDEN,
        ));
    }
    Ok(())
}
async fn allowed(app: &App, principal: &Principal, group: &str, capability: Capability) -> bool {
    app.allows(group).await && principal.allows(&*app.clients.read().await, group, capability)
}

#[derive(Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct ClientRequest {
    name: Option<String>,
    enabled: Option<bool>,
    groups: Option<BTreeSet<String>>,
    group_patterns: Option<BTreeSet<String>>,
    can_send: Option<bool>,
    can_download: Option<bool>,
}
fn apply_client_request(access: &mut ClientAccess, request: ClientRequest) -> ApiResult<()> {
    if let Some(name) = request.name {
        access.name = name.trim().to_owned();
    }
    if access.name.is_empty()
        || access.name.chars().count() > 64
        || access.name.chars().any(char::is_control)
    {
        bail_api("客户端昵称需要 1 至 64 个可显示字符")?;
    }
    if let Some(groups) = request.groups {
        access.groups = groups;
    }
    if let Some(patterns) = request.group_patterns {
        crate::group_rules::validate(&patterns)?;
        access.group_patterns = patterns;
    }
    if access.groups.len() > 2000
        || access
            .groups
            .iter()
            .any(|id| id.is_empty() || id.len() > 512)
    {
        bail_api("授权群列表无效或过长")?;
    }
    if let Some(enabled) = request.enabled {
        if access.revoked && enabled {
            bail_api("已撤销的密钥不能恢复，请创建新密钥")?;
        }
        access.enabled = enabled;
    }
    if let Some(value) = request.can_send {
        access.can_send = value;
    }
    if let Some(value) = request.can_download {
        access.can_download = value;
    }
    Ok(())
}
async fn persist_client(app: &App, record: &mut ClientRecord, create: bool) -> ApiResult<ClientAccess> {
    record.resolve_groups(&app.db(|store| store.groups()).await?);
    let events = app
        .db({
            let record = record.clone();
            move |store| store.save_client(&record, create)
        })
        .await?;
    // Caller holds the registry write lock, so no action can be newly submitted between
    // the durable permission change and its in-memory publication.
    for event in events {
        app.publish(event);
    }
    Ok(record.access.clone())
}
async fn local_clients(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    let clients = app.clients.read().await;
    Ok(Json(
        json!({"clients":clients.values().map(|record| &record.access).collect::<Vec<_>>()}),
    ))
}
async fn create_client(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Json(request): Json<ClientRequest>,
) -> ApiResult<Response> {
    check_admin(&principal, address)?;
    let now = chrono::Utc::now().timestamp_millis();
    let mut access = ClientAccess {
        id: uuid::Uuid::new_v4().to_string(),
        name: String::new(),
        enabled: true,
        revoked: false,
        groups: BTreeSet::new(),
        group_patterns: BTreeSet::new(),
        can_send: false,
        can_download: false,
        created_at: now,
        updated_at: now,
        revision: 1,
    };
    apply_client_request(&mut access, request)?;
    let token = format!(
        "wxcl_{}{}",
        uuid::Uuid::new_v4().simple(),
        uuid::Uuid::new_v4().simple()
    );
    let mut record = ClientRecord {
        access,
        token_hash: access::token_hash(&token),
        token: Some(token.clone()),
        matched_groups: BTreeSet::new(),
    };
    let mut clients = app.clients.write().await;
    let access = persist_client(&app, &mut record, true).await?;
    clients.insert(access.id.clone(), record);
    app.clients_changed.send_replace(());
    Ok(credential_response(access, Some(token)))
}
fn credential_response(access: ClientAccess, token: Option<String>) -> Response {
    (
        [(header::CACHE_CONTROL, "no-store")],
        Json(json!({"client":access,"token":token})),
    )
        .into_response()
}
async fn client_credential(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    check_admin(&principal, address)?;
    let clients = app.clients.read().await;
    let record = clients
        .get(&id)
        .ok_or_else(|| Error(anyhow::anyhow!("客户端不存在"), StatusCode::NOT_FOUND))?;
    if record.access.revoked {
        return Err(Error(
            anyhow::anyhow!("已撤销的密钥无法获取或轮换"),
            StatusCode::FORBIDDEN,
        ));
    }
    Ok(credential_response(
        record.access.clone(),
        record.token.clone(),
    ))
}
async fn rotate_client(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    check_admin(&principal, address)?;
    let mut clients = app.clients.write().await;
    let mut record = clients
        .get(&id)
        .cloned()
        .ok_or_else(|| Error(anyhow::anyhow!("客户端不存在"), StatusCode::NOT_FOUND))?;
    if record.access.revoked {
        return Err(Error(
            anyhow::anyhow!("已撤销的密钥无法获取或轮换"),
            StatusCode::FORBIDDEN,
        ));
    }
    let token = format!(
        "wxcl_{}{}",
        uuid::Uuid::new_v4().simple(),
        uuid::Uuid::new_v4().simple()
    );
    record.token_hash = access::token_hash(&token);
    record.token = Some(token.clone());
    record.access.updated_at = chrono::Utc::now().timestamp_millis();
    record.access.revision += 1;
    let access = persist_client(&app, &mut record, false).await?;
    clients.insert(id, record);
    app.clients_changed.send_replace(());
    Ok(credential_response(access, Some(token)))
}
async fn update_client(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Path(id): Path<String>,
    Json(request): Json<ClientRequest>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    let mut clients = app.clients.write().await;
    let mut record = clients.get(&id).cloned().context("客户端不存在")?;
    apply_client_request(&mut record.access, request)?;
    record.access.updated_at = chrono::Utc::now().timestamp_millis();
    record.access.revision += 1;
    let access = persist_client(&app, &mut record, false).await?;
    clients.insert(id, record);
    app.clients_changed.send_replace(());
    Ok(Json(json!({"client":access})))
}
async fn revoke_client(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    let mut clients = app.clients.write().await;
    let mut record = clients.get(&id).cloned().context("客户端不存在")?;
    record.access.revoked = true;
    record.access.enabled = false;
    record.token = None;
    record.access.updated_at = chrono::Utc::now().timestamp_millis();
    record.access.revision += 1;
    let access = persist_client(&app, &mut record, false).await?;
    clients.insert(id, record);
    app.clients_changed.send_replace(());
    Ok(Json(json!({"client":access})))
}
fn check_group(selected: &BTreeSet<String>, group: &str) -> ApiResult<()> {
    if !selected.contains(group) {
        return Err(Error(
            anyhow::anyhow!("该群未获准转发"),
            StatusCode::FORBIDDEN,
        ));
    }
    Ok(())
}
async fn forwarding_snapshot(app: &App) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    let (groups, manual, patterns) = app.db(|s| Ok((
        s.group_summaries()?, s.forward_groups()?.unwrap_or_default(), s.forward_patterns()?
    ))).await?;
    Ok(Json(json!({"groups":groups,"selected":*selected,"manual_selected":manual,"patterns":patterns})))
}
async fn local_forwarding(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    forwarding_snapshot(&app).await
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ForwardingRequest {
    selected: Vec<String>,
    patterns: Option<BTreeSet<String>>,
}
async fn set_forwarding(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    ConnectInfo(address): ConnectInfo<SocketAddr>,
    Json(request): Json<ForwardingRequest>,
) -> ApiResult<Json<Value>> {
    check_admin(&principal, address)?;
    if request.selected.len() > 2000
        || request
            .selected
            .iter()
            .any(|id| id.is_empty() || id.len() > 512)
    {
        bail_api("所选群列表无效或过长")?;
    }
    app.set_forwarding_policy(request.selected.into_iter().collect(), request.patterns)
        .await?;
    forwarding_snapshot(&app).await
}
async fn groups(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    let clients = app.clients.read().await;
    let mut groups = app.db(|s| s.group_summaries()).await?;
    groups.retain(|g| {
        g["id"].as_str().is_some_and(|id| {
            selected.contains(id) && principal.allows(&clients, id, Capability::Read)
        })
    });
    Ok(Json(json!(groups)))
}
async fn members(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(group): Path<String>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    check_group(&selected, &group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &group, Capability::Read)?;
    Ok(Json(json!(app.db(move |s| s.members(&group)).await?)))
}
async fn activate(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(group): Path<String>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    check_group(&selected, &group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &group, Capability::Read)?;
    if app.status.read().await["ready"] != true {
        bail_api("本机微信未连接")?;
    }
    let id = group.clone();
    if !app
        .db(move |s| Ok(s.groups()?.iter().any(|g| g.id == id)))
        .await?
    {
        bail_api("群不存在")?;
    }
    app.commands
        .try_send(json!({"op":"activate","group":group,"_access_client":principal.client_id()}))
        .map_err(|_| anyhow::anyhow!("微信连接不可用或任务队列已满"))?;
    Ok(Json(json!({"ok":true})))
}
fn bail_api(text: &str) -> ApiResult<()> {
    Err(Error(
        anyhow::anyhow!(text.to_owned()),
        StatusCode::BAD_REQUEST,
    ))
}

#[derive(Default, Deserialize)]
struct HistoryQuery {
    group: String,
    #[serde(default)]
    before: i64,
    #[serde(default)]
    search: String,
}
async fn history(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Query(query): Query<HistoryQuery>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Read)?;
    if query.search.len() > 512 {
        bail_api("搜索词过长")?;
    }
    Ok(Json(public(json!(
        app.db(move |s| s.history(&query.group, query.before, &query.search))
            .await?
    ))))
}

async fn native_history(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Query(query): Query<HistoryQuery>,
) -> ApiResult<Json<Value>> {
    if query.before < 0 || !query.search.is_empty() {
        bail_api("历史游标无效；搜索请使用消息搜索接口")?;
    }
    {
        let selected = app.forwarding.read().await;
        check_group(&selected, &query.group)?;
        let clients = app.clients.read().await;
        check_access(&clients, &principal, &query.group, Capability::Read)?;
        let (cached, plan, messages) = app
            .db({
                let group = query.group.clone();
                let before = query.before;
                move |store| {
                    Ok((
                        store.history(&group, before, "")?,
                        store.history_progress(&group)?,
                        store.history_with_imports(&group, before)?,
                    ))
                }
            })
            .await?;
        if cached.len() >= 60 || app.demo || plan.exhausted {
            return Ok(Json(public(
                json!({"messages":messages,"has_more":cached.len()>=60 || plan.pending,"pending":plan.pending}),
            )));
        }
        if app.status.read().await["ready"] != true && !cached.is_empty() {
            return Ok(Json(public(
                json!({"messages":messages,"has_more":true,"pending":true}),
            )));
        }
    }
    if app.status.read().await["ready"] != true {
        return Err(Error(
            anyhow::anyhow!("本机微信未连接，无法读取更早历史，请稍后重试"),
            StatusCode::SERVICE_UNAVAILABLE,
        ));
    }
    let receiver = app
        .history
        .request(&app, &query.group, principal.client_id(), query.before == 0)
        .await
        .map_err(|error| Error(error, StatusCode::SERVICE_UNAVAILABLE))?;
    let mut pending = false;
    let mut history_error = None;
    if let Some(mut receiver) = receiver {
        let result = tokio::time::timeout(Duration::from_secs(3), async {
            loop {
                if let Some(value) = receiver.borrow_and_update().clone() {
                    return Some(value);
                }
                if receiver.changed().await.is_err() {
                    return None;
                }
            }
        })
        .await;
        match result {
            Ok(Some(value)) if !value["error"].as_str().unwrap_or("").is_empty() => {
                history_error = Some(value["error"].as_str().unwrap().to_owned())
            }
            Ok(Some(_)) => {}
            _ => pending = true,
        }
    }
    // No authorization lock is held during native I/O: a mid-request revoke must win.
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Read)?;
    if let Some(error) = history_error {
        return Err(Error(
            anyhow::anyhow!(error),
            StatusCode::SERVICE_UNAVAILABLE,
        ));
    }
    let (messages, progress, cached_count) = app
        .db(move |store| {
            Ok((
                store.history_with_imports(&query.group, query.before)?,
                store.history_progress(&query.group)?,
                store.history(&query.group, query.before, "")?.len(),
            ))
        })
        .await?;
    let pending = pending || progress.pending;
    Ok(Json(public(
        json!({"messages":messages,"has_more":pending || !progress.exhausted || cached_count>=60,"pending":pending}),
    )))
}

async fn announcement(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(group): Path<String>,
) -> ApiResult<Json<Value>> {
    {
        let selected = app.forwarding.read().await;
        check_group(&selected, &group)?;
        let clients = app.clients.read().await;
        check_access(&clients, &principal, &group, Capability::Read)?;
    }
    if app.demo || app.status.read().await["ready"] != true {
        return Ok(Json(crate::announcement::unavailable(&group, false)));
    }
    let (id, mut receiver) = app
        .announcements
        .request(&app, &group, principal.client_id())
        .await
        .map_err(|e| Error(e, StatusCode::SERVICE_UNAVAILABLE))?;
    let received = tokio::time::timeout(crate::announcement::TIMEOUT, async {
        loop {
            if let Some(value) = receiver.borrow_and_update().clone() {
                return Some(value);
            }
            if receiver.changed().await.is_err() {
                return None;
            }
        }
    })
    .await;
    let response = match received {
        Ok(Some(value)) => value,
        _ => {
            app.announcements.expire(&group, &id).await;
            crate::announcement::unavailable(&group, true)
        }
    };
    let selected = app.forwarding.read().await;
    check_group(&selected, &group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &group, Capability::Read)?;
    Ok(Json(response))
}

fn record_response(
    mut scan: crate::records::Scan,
    cursor: crate::records::Cursor,
    progress: &crate::history::Progress,
    demo: bool,
    waiting: bool,
) -> Json<Value> {
    scan.messages
        .sort_by_key(|message| std::cmp::Reverse((message.timestamp, message.seq)));
    let pending = waiting || progress.pending;
    let has_more = scan.cached_more || pending || (!demo && !progress.exhausted);
    Json(public(
        json!({"messages":scan.messages,"next_before":if has_more {Some(cursor.encode())} else {None},"has_more":has_more,"pending":pending}),
    ))
}

async fn records(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Query(query): Query<crate::records::Query>,
) -> ApiResult<Json<Value>> {
    query.validate()?;
    let (mut cursor, scan, progress) = {
        let selected = app.forwarding.read().await;
        check_group(&selected, &query.group)?;
        let clients = app.clients.read().await;
        check_access(&clients, &principal, &query.group, Capability::Read)?;
        app.db({
            let query = query.clone();
            move |store| {
                let progress = store.history_progress(&query.group)?;
                let mut cursor = crate::records::Cursor::load(
                    &query,
                    store.record_snapshot(&query.group)?,
                    &progress,
                )?;
                let scan =
                    crate::records::scan(store, &query, &mut cursor, crate::records::SCAN_LIMIT)?;
                cursor.set_progress(&progress);
                Ok((cursor, scan, progress))
            }
        })
        .await?
    };
    // A cached scan, including a page with no matches, always advances its keyset.
    // Pull only after that cache window is consumed; one HTTP request submits at most one native page.
    if scan.scanned > 0 || scan.cached_more || app.demo || progress.exhausted {
        let selected = app.forwarding.read().await;
        check_group(&selected, &query.group)?;
        let clients = app.clients.read().await;
        check_access(&clients, &principal, &query.group, Capability::Read)?;
        return Ok(record_response(scan, cursor, &progress, app.demo, false));
    }
    if app.status.read().await["ready"] != true {
        return Err(Error(
            anyhow::anyhow!("本机微信未连接，无法读取更早历史，请稍后重试"),
            StatusCode::SERVICE_UNAVAILABLE,
        ));
    }
    let receiver = app
        .history
        .request(
            &app,
            &query.group,
            principal.client_id(),
            query.before.is_empty(),
        )
        .await
        .map_err(|e| Error(e, StatusCode::SERVICE_UNAVAILABLE))?;
    let mut waiting = false;
    let mut history_error = None;
    if let Some(mut receiver) = receiver {
        match tokio::time::timeout(Duration::from_secs(3), async {
            loop {
                if let Some(value) = receiver.borrow_and_update().clone() {
                    return Some(value);
                }
                if receiver.changed().await.is_err() {
                    return None;
                }
            }
        })
        .await
        {
            Ok(Some(value)) => {
                history_error = value["error"]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .map(str::to_owned)
            }
            _ => waiting = true,
        }
    }
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Read)?;
    if let Some(error) = history_error {
        return Err(Error(
            anyhow::anyhow!(error),
            StatusCode::SERVICE_UNAVAILABLE,
        ));
    }
    let (scan, progress, cursor) = app
        .db(move |store| {
            let progress = store.history_progress(&query.group)?;
            let scan =
                crate::records::scan(store, &query, &mut cursor, crate::records::SCAN_LIMIT)?;
            cursor.set_progress(&progress);
            Ok((scan, progress, cursor))
        })
        .await?;
    Ok(record_response(scan, cursor, &progress, app.demo, waiting))
}

#[derive(Deserialize)]
struct LocateQuery {
    group: String,
}
async fn locate(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(id): Path<String>,
    Query(query): Query<LocateQuery>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Read)?;
    let message = app
        .db(move |s| s.message(&id))
        .await?
        .filter(|message| message.group == query.group)
        .ok_or_else(|| {
            Error(
                anyhow::anyhow!("原消息未同步或不存在"),
                StatusCode::NOT_FOUND,
            )
        })?;
    if message.status == "recalled" {
        return Err(Error(anyhow::anyhow!("原消息已撤回"), StatusCode::GONE));
    }
    Ok(Json(public(json!(message))))
}

async fn refresh_quote_source(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(id): Path<String>,
    Query(query): Query<LocateQuery>,
) -> ApiResult<Json<Value>> {
    {
        let selected = app.forwarding.read().await;
        check_group(&selected, &query.group)?;
        let clients = app.clients.read().await;
        check_access(&clients, &principal, &query.group, Capability::Read)?;
    }
    let message = app.db(move |s| s.message(&id)).await?
        .filter(|message| message.group == query.group)
        .ok_or_else(|| Error(anyhow::anyhow!("引用消息不存在"), StatusCode::NOT_FOUND))?;
    let quote = message.quote.as_ref().context("该消息没有引用")?;
    if message.status == "recalled" || quote.unavailable {
        return Err(Error(anyhow::anyhow!("原消息已撤回或不可用"), StatusCode::GONE));
    }
    let original = app.db({ let id = quote.id.clone(); move |s| s.message(&id) }).await?;
    if let Some(original) = original.filter(|original| original.group == query.group && original.status != "recalled") {
        return Ok(Json(public(json!({"message":message,"original":original}))));
    }
    if app.status.read().await["ready"] != true {
        return Err(Error(anyhow::anyhow!("本机微信未连接"), StatusCode::SERVICE_UNAVAILABLE));
    }
    if crate::history::position(&query.group, &message.source_id).is_none() {
        return Err(Error(anyhow::anyhow!("引用来源尚未确认"), StatusCode::CONFLICT));
    }
    let request_id = uuid::Uuid::new_v4().to_string();
    let mut events = app.events.subscribe();
    app.commands.try_send(json!({"op":"quote_refresh","request_id":request_id,"group":query.group,
        "source_id":message.source_id,"_access_client":principal.client_id()}))
        .map_err(|_| Error(anyhow::anyhow!("引用读取任务较多，请稍后重试"), StatusCode::SERVICE_UNAVAILABLE))?;
    let completed = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match events.recv().await {
                Ok(event) if event["type"] == "quote_refresh_done" && event["request_id"] == request_id
                    && event["group"] == query.group && event["source_id"] == message.source_id => break,
                Ok(_) | Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {},
                Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
            }
        }
    }).await.is_ok();
    // Revocation while the adapter was reading must win over the completed result.
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Read)?;
    let updated = app.db({ let id = message.id; move |s| s.message(&id) }).await?
        .filter(|updated| updated.group == query.group && updated.source_id == message.source_id)
        .ok_or_else(|| Error(anyhow::anyhow!("引用消息不存在"), StatusCode::NOT_FOUND))?;
    let quote = updated.quote.as_ref().context("该消息没有引用")?;
    if updated.status == "recalled" || quote.unavailable {
        return Err(Error(anyhow::anyhow!("原消息已撤回或不可用"), StatusCode::GONE));
    }
    let original = app.db({ let id = quote.id.clone(); move |s| s.message(&id) }).await?
        .filter(|original| original.group == query.group && original.status != "recalled")
        .ok_or_else(|| Error(anyhow::anyhow!(if completed { "原消息尚未同步或不可用" } else { "引用原文读取超时，请重试" }),
            if completed { StatusCode::NOT_FOUND } else { StatusCode::GATEWAY_TIMEOUT }))?;
    Ok(Json(public(json!({"message":updated,"original":original}))))
}

#[derive(Deserialize)]
struct SendRequest {
    id: String,
    group: String,
    client_id: String,
    sender: String,
    #[serde(default)]
    text: String,
    #[serde(default)]
    media: Vec<String>,
    #[serde(default)]
    mentions: Vec<Mention>,
    #[serde(default)]
    quote: Option<Quote>,
}
async fn send(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Json(request): Json<SendRequest>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    check_group(&selected, &request.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &request.group, Capability::Send)?;
    uuid::Uuid::parse_str(&request.id).context("发送编号无效")?;
    if request.text.len() > 24_000
        || request.text.contains('\0')
        || request.sender.trim().is_empty()
        || request.sender.chars().count() > 64
        || request.client_id.len() > 80
        || request.client_id.is_empty()
        || request.media.len() > 12
        || request.mentions.len() > 100
        || (request.text.trim().is_empty() && request.media.is_empty())
    {
        bail_api("消息为空或超过容量限制")?;
    }
    if let Some(existing) = app
        .db({
            let id = request.id.clone();
            move |s| s.message(&id)
        })
        .await?
    {
        if existing.group != request.group
            || existing.access_client != principal.client_id()
            || existing.client_id != request.client_id
            || existing.text != request.text
            || existing
                .media
                .iter()
                .map(|m| m.id.as_str())
                .collect::<Vec<_>>()
                != request.media.iter().map(String::as_str).collect::<Vec<_>>()
            || serde_json::to_value(&existing.mentions)? != serde_json::to_value(&request.mentions)?
            || existing.quote.as_ref().map(|q| &q.id) != request.quote.as_ref().map(|q| &q.id)
        {
            bail_api("重复发送编号的内容不一致")?;
        }
        return Ok(Json(public(json!(existing))));
    }
    if app.status.read().await["ready"] != true {
        return Err(Error(
            anyhow::anyhow!("本机微信未连接，消息未提交"),
            StatusCode::BAD_REQUEST,
        ));
    }
    let group = request.group.clone();
    let mentions = request.mentions.clone();
    let text = request.text.clone();
    let ids = request.media.clone();
    let requested_quote = request.quote.clone();
    let (media, quote) = app
        .db(move |s| {
            if !s.groups()?.iter().any(|g| g.id == group) {
                bail!("群不存在");
            }
            let members = s.members(&group)?;
            for mention in &mentions {
                if !members
                    .iter()
                    .any(|m| m.id == mention.id && m.name == mention.name)
                    || !text.contains(&format!("@{}", mention.name))
                {
                    bail!("@ 成员已改变，请重新选择");
                }
            }
            let media = ids
                .iter()
                .map(|id| s.media_for_group(id, &group))
                .collect::<Result<Vec<_>>>()?;
            let quote = requested_quote
                .as_ref()
                .map(|quote| s.quote(&group, quote))
                .transpose()?;
            Ok((media, quote))
        })
        .await?;
    if let Some(quote) = &quote {
        if quote.text.len() > 24_000 || quote.text.contains('\0') {
            bail_api("引用原文超过容量限制或包含无效字符")?;
        }
    }
    let permit = app
        .commands
        .try_reserve()
        .map_err(|_| anyhow::anyhow!("发送队列已满，请稍后再试"))?;
    let mut message = ChatMessage {
        seq: 0,
        id: request.id,
        group: request.group,
        sender: request.sender.trim().into(),
        avatar: String::new(),
        sender_id: String::new(),
        client_id: request.client_id,
        access_client: principal.client_id().to_owned(),
        recall_client: String::new(),
        text: request.text,
        media,
        mentions: request.mentions,
        quote,
        record: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
        status: "queued".into(),
        error: String::new(),
        source_id: String::new(),
        source_ids: Vec::new(),
        recall_status: String::new(),
        recall_error: String::new(),
        recall_retryable: false,
        recall_attempt: String::new(),
        outgoing: true,
    };
    let (stored, event) = app
        .db(move |s| {
            let event = s.insert_message(&mut message)?;
            Ok((message, event))
        })
        .await?;
    if let Some(event) = event {
        app.publish(event);
        // The durable queue is authoritative; this command only wakes its consumer.
        permit.send(json!({"op":"send"}));
    }
    Ok(Json(public(json!(stored))))
}

async fn recall(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    let selected = app.forwarding.read().await;
    let existing = app
        .db({
            let id = id.clone();
            move |s| s.message(&id)
        })
        .await?
        .context("消息不存在")?;
    check_group(&selected, &existing.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &existing.group, Capability::Send)?;
    if !existing.outgoing {
        bail_api("只能撤回本人发送的消息")?;
    }
    {
        let status = app.status.read().await;
        if let Some(reason) = crate::model::recall_unavailable(&status) {
            return Err(Error(
                anyhow::anyhow!(reason.to_owned()),
                StatusCode::NOT_IMPLEMENTED,
            ));
        }
    }
    let explicit_retry = existing.recall_status == "failed" && existing.recall_retryable;
    if existing.status == "recalled" || (!existing.recall_status.is_empty() && !explicit_retry) {
        return Ok(Json(public(json!(existing))));
    }
    if app.status.read().await["ready"] != true {
        bail_api("本机微信未连接，撤回未提交")?;
    }
    let permit = app
        .commands
        .try_reserve()
        .map_err(|_| anyhow::anyhow!("任务队列已满，请稍后再试"))?;
    let (message, event) = app
        .db(move |s| {
            s.begin_recall_as(
                &id,
                chrono::Utc::now().timestamp_millis(),
                principal.client_id(),
            )
        })
        .await?;
    if let Some(event) = event {
        app.publish(event);
        permit.send(json!({"op":"recall","recall_attempt":message.recall_attempt,"message":message,"explicit_retry":explicit_retry}));
    }
    Ok(Json(public(json!(message))))
}

#[derive(Deserialize)]
struct UploadQuery {
    name: String,
    group: String,
}
pub(crate) struct PartialFile(pub(crate) PathBuf);
impl PartialFile {
    pub(crate) fn keep(mut self) {
        self.0.clear();
    }
}
impl Drop for PartialFile {
    fn drop(&mut self) {
        if !self.0.as_os_str().is_empty() {
            let _ = std::fs::remove_file(&self.0);
        }
    }
}

async fn upload(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Query(query): Query<UploadQuery>,
    headers: HeaderMap,
    body: Body,
) -> ApiResult<Json<Value>> {
    check_group(&*app.forwarding.read().await, &query.group)?;
    check_access(
        &*app.clients.read().await,
        &principal,
        &query.group,
        Capability::Send,
    )?;
    let _permit = app
        .transfers
        .try_acquire()
        .map_err(|_| anyhow::anyhow!("正在传输的附件较多，请稍后再试"))?;
    let expected: u64 = headers
        .get(header::CONTENT_LENGTH)
        .and_then(|h| h.to_str().ok())
        .context("需要附件长度")?
        .parse()?;
    if expected == 0 || expected > MAX_FILE {
        bail_api("附件需要大于 0 且不超过 512 MiB")?;
    }
    let name = clean_name(&query.name);
    let id = uuid::Uuid::new_v4().to_string();
    let day = chrono::Local::now().format("%Y-%m-%d").to_string();
    let directory = app.config.data.join("media").join(day);
    tokio::fs::create_dir_all(&directory).await?;
    let path = directory.join(format!("{id}_{name}"));
    let mut partial = PartialFile(path.with_extension("part"));
    let mut file = tokio::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&partial.0)
        .await?;
    let mut stream = body.into_data_stream();
    let mut length = 0;
    let mut hash = Sha256::new();
    let mut head = Vec::new();
    while let Some(chunk) = stream.next().await {
        check_group(&*app.forwarding.read().await, &query.group)?;
        check_access(
            &*app.clients.read().await,
            &principal,
            &query.group,
            Capability::Send,
        )?;
        let chunk = chunk?;
        length += chunk.len() as u64;
        if length > expected || length > MAX_FILE {
            bail_api("附件长度超过声明值")?;
        }
        if head.len() < 32 {
            head.extend_from_slice(&chunk[..chunk.len().min(32 - head.len())]);
        }
        hash.update(&chunk);
        file.write_all(&chunk).await?;
    }
    if length != expected {
        bail_api("附件未完整上传")?;
    }
    file.sync_all().await?;
    drop(file);
    let selected = app.forwarding.read().await;
    check_group(&selected, &query.group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &query.group, Capability::Send)?;
    tokio::fs::rename(&partial.0, &path).await?;
    partial.0 = path.clone();
    let media = Media {
        id,
        name: name.clone(),
        kind: detect_kind(&name, &head).into(),
        size: length,
        sha256: format!("{:x}", hash.finalize()),
        path: path.to_string_lossy().into_owned(),
    };
    app.db({
        let media = media.clone();
        move |s| s.set_media(&media, &query.group)
    })
    .await?;
    partial.keep();
    Ok(Json(public(json!(media))))
}

async fn download(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    let permit = app
        .transfers
        .clone()
        .try_acquire_owned()
        .map_err(|_| anyhow::anyhow!("正在传输的附件较多，请稍后再试"))?;
    let (group, media) = app
        .db(move |s| Ok((s.media_group(&id)?, s.media(&id)?)))
        .await?;
    let selected = app.forwarding.read().await;
    check_group(&selected, &group)?;
    let clients = app.clients.read().await;
    check_access(&clients, &principal, &group, Capability::Download)?;
    let path = tokio::fs::canonicalize(&media.path).await?;
    let root = tokio::fs::canonicalize(app.config.data.join("media")).await?;
    if !path.starts_with(root) {
        bail_api("附件路径无效")?;
    }
    let file = tokio::fs::File::open(path).await?;
    let stream = ReaderStream::with_capacity(file, 64 * 1024);
    let live_app = app.clone();
    let stream = futures_util::stream::unfold(
        (stream, permit, live_app, group, principal),
        |(mut stream, permit, app, group, principal)| async move {
            if !allowed(&app, &principal, &group, Capability::Download).await {
                return None;
            }
            let chunk = stream.next().await?;
            if !allowed(&app, &principal, &group, Capability::Download).await {
                return None;
            }
            Some((chunk, (stream, permit, app, group, principal)))
        },
    );
    Ok(Response::builder()
        .header(header::CONTENT_TYPE, "application/octet-stream")
        .header(header::CONTENT_LENGTH, media.size)
        .header("X-Content-SHA256", media.sha256)
        .body(Body::from_stream(stream))?)
}

async fn video_poster(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Path(id): Path<String>,
) -> ApiResult<Response> {
    let (group, media) = app
        .db(move |s| Ok((s.media_group(&id)?, s.media(&id)?)))
        .await?;
    if !allowed(&app, &principal, &group, Capability::Download).await {
        return Err(Error(
            anyhow::anyhow!("没有此视频的下载权限"),
            StatusCode::FORBIDDEN,
        ));
    }
    let bytes = crate::poster::load(&app, &media).await?;
    // Recheck after decoding: revocation must also apply to a small cover.
    if !allowed(&app, &principal, &group, Capability::Download).await {
        return Err(Error(
            anyhow::anyhow!("此视频的下载权限已撤销"),
            StatusCode::FORBIDDEN,
        ));
    }
    Ok(Response::builder()
        .header(header::CONTENT_TYPE, "image/jpeg")
        .header(header::CACHE_CONTROL, "no-store")
        .header(header::CONTENT_LENGTH, bytes.len())
        .header("X-Content-SHA256", format!("{:x}", Sha256::digest(&bytes)))
        .body(Body::from(bytes))?)
}

#[derive(Default, Deserialize)]
struct EventQuery {
    #[serde(default)]
    after: i64,
}
async fn events(
    State(app): State<Arc<App>>,
    Extension(principal): Extension<Principal>,
    Query(query): Query<EventQuery>,
    ws: WebSocketUpgrade,
) -> Response {
    ws.max_message_size(64 * 1024)
        .on_upgrade(move |socket| event_stream(app, principal, socket, query.after))
}
async fn emit(socket: &mut WebSocket, value: Value) -> bool {
    matches!(
        tokio::time::timeout(
            Duration::from_secs(8),
            socket.send(Message::Text(public(value).to_string().into()))
        )
        .await,
        Ok(Ok(()))
    )
}
async fn wait_access_changed(app: &App, principal: &Principal) {
    let mut changes = app.clients_changed.subscribe();
    loop {
        if !principal.valid(&*app.clients.read().await) {
            return;
        }
        if changes.changed().await.is_err() {
            return;
        }
    }
}
struct OnlineClient(Arc<App>, String);
impl OnlineClient {
    fn new(app: Arc<App>, principal: &Principal) -> Self {
        let id = principal.client_id().to_owned();
        if !id.is_empty() {
            *app.online_clients
                .lock()
                .unwrap()
                .entry(id.clone())
                .or_default() += 1;
        }
        Self(app, id)
    }
}
impl Drop for OnlineClient {
    fn drop(&mut self) {
        let mut online = self.0.online_clients.lock().unwrap();
        if let Some(count) = online.get_mut(&self.1) {
            *count -= 1;
            if *count == 0 {
                online.remove(&self.1);
            }
        }
    }
}
async fn event_stream(app: Arc<App>, principal: Principal, mut socket: WebSocket, after: i64) {
    let _online = OnlineClient::new(app.clone(), &principal);
    tokio::select! {
        _ = crate::wait_for_shutdown(app.shutdown.subscribe()) => {},
        _ = wait_access_changed(&app, &principal) => {},
        _ = stream_events(app.clone(), &principal, &mut socket, after) => return,
    }
    let _ = tokio::time::timeout(Duration::from_secs(1), socket.send(Message::Close(None))).await;
}
async fn replay(app: &App, principal: &Principal, socket: &mut WebSocket, after: &mut i64) -> bool {
    loop {
        let cursor = *after;
        let Ok(batch) = app.db(move |s| s.events(cursor)).await else {
            return false;
        };
        let count = batch.len();
        for event in batch {
            *after = event["seq"].as_i64().unwrap_or(*after);
            if visible_event(app, principal, &event).await && !emit(socket, event).await {
                return false;
            }
        }
        if count < 256 {
            return true;
        }
    }
}
async fn visible_event(app: &App, principal: &Principal, event: &Value) -> bool {
    if event["type"] == "message" {
        return allowed(
            app,
            principal,
            event["message"]["group"].as_str().unwrap_or(""),
            Capability::Read,
        )
        .await;
    }
    if let Some(group) = event["group"].as_str() {
        return allowed(app, principal, group, Capability::Read).await;
    }
    principal.valid(&*app.clients.read().await)
        && matches!(
            event["type"].as_str(),
            Some("status" | "groups" | "warning")
        )
}
async fn stream_events(
    app: Arc<App>,
    principal: &Principal,
    socket: &mut WebSocket,
    mut after: i64,
) {
    let mut receiver = app.events.subscribe();
    let Ok(head) = app.db(|s| s.event_head()).await else {
        return;
    };
    if after > head {
        if !emit(socket, json!({"type":"reset","seq":0})).await {
            return;
        }
        after = head;
    } else if after < 0 {
        after = head;
    }
    let status = principal_status(&app, principal).await;
    if !emit(socket, status).await {
        return;
    }
    if !replay(&app, principal, socket, &mut after).await {
        return;
    }
    if !emit(socket, json!({"type":"synced","seq":after})).await {
        return;
    }
    let mut heartbeat = tokio::time::interval(Duration::from_secs(30));
    heartbeat.tick().await;
    loop {
        tokio::select! {
            event=receiver.recv()=>match event {
                Ok(event)=>{
                    let seq=event["seq"].as_i64().unwrap_or(0);
                    if seq>0 {
                        if seq<=after {continue;}
                        // Concurrent publishers can wake in a different order than their commits.
                        if !replay(&app,principal,socket,&mut after).await {return;}
                    } else if visible_event(&app,principal,&event).await {
                        let event=if event["type"]=="status" {principal_status(&app,principal).await} else {event};
                        if !emit(socket,event).await {return;}
                    }
                },
                Err(_)=>{ let _=socket.send(Message::Close(None)).await;return; }
            },
            incoming=socket.recv()=>match incoming {Some(Ok(Message::Close(_)))|None|Some(Err(_))=>return,_=>{}},
            _=heartbeat.tick()=>{if socket.send(Message::Ping(Vec::new().into())).await.is_err(){return;}}
        }
    }
}

pub async fn import_media(
    app: &App,
    group: &str,
    source: &str,
    name: &str,
    kind: &str,
) -> Result<Media> {
    if !app.allows(group).await {
        bail!("该群未获准转发");
    }
    let path = std::path::Path::new(source);
    let name = clean_name(name);
    if !path.is_absolute() {
        bail!("本机附件路径必须为绝对路径");
    }
    let mut input = tokio::fs::File::open(path).await?;
    let size = input.metadata().await?.len();
    if size == 0 || size > MAX_FILE {
        bail!("微信附件为空或超过容量");
    }
    let id = uuid::Uuid::new_v4().to_string();
    let directory = app
        .config
        .data
        .join("media")
        .join(chrono::Local::now().format("%Y-%m-%d").to_string());
    tokio::fs::create_dir_all(&directory).await?;
    let target = directory.join(format!("{id}_{name}"));
    let mut partial = PartialFile(target.with_extension("part"));
    let mut output = tokio::fs::File::create(&partial.0).await?;
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 64 * 1024];
    let mut total = 0;
    loop {
        if !app.allows(group).await {
            bail!("该群未获准转发");
        }
        let n = input.read(&mut buffer).await?;
        if n == 0 {
            break;
        }
        total += n as u64;
        if total > MAX_FILE {
            bail!("附件写入中仍在增长");
        }
        hash.update(&buffer[..n]);
        output.write_all(&buffer[..n]).await?;
    }
    if size != total {
        bail!("附件在读取时发生变化");
    }
    output.sync_all().await?;
    drop(output);
    let selected = app.forwarding.read().await;
    if !selected.contains(group) {
        bail!("该群未获准转发");
    }
    tokio::fs::rename(&partial.0, &target).await?;
    partial.0 = target.clone();
    let media = Media {
        id,
        name,
        kind: kind.into(),
        size,
        sha256: format!("{:x}", hash.finalize()),
        path: target.to_string_lossy().into_owned(),
    };
    app.db({
        let media = media.clone();
        let group = group.to_owned();
        move |s| s.set_media(&media, &group)
    })
    .await?;
    partial.keep();
    Ok(media)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tower::ServiceExt;

    #[tokio::test]
    async fn wildcard_rules_follow_new_groups_and_renames_with_independent_client_access() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _) = crate::test_app(temp.path());
        grant(&app).await;
        let router = router(app.clone());
        let call = |method: &str, path: &str, token: &str, body: Value| {
            router.clone().oneshot(request(method, path, token, "127.0.0.1:2345", Body::from(body.to_string())))
        };
        let pattern = "*-PLY* - jx";
        let response = call("POST", "/api/local/forwarding", &app.config.token,
            json!({"selected":["g"],"patterns":[pattern]})).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(json_body(response).await["selected"], json!(["g"]));
        let response = call("POST", "/api/local/clients", &app.config.token,
            json!({"name":"Rule client","groups":[],"group_patterns":[pattern],"can_download":true})).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let credential = json_body(response).await;
        let token = credential["token"].as_str().unwrap();
        let client_id = credential["client"]["id"].as_str().unwrap();
        let group = |id: &str, name: &str| crate::model::Group {
            id: id.into(), name: name.into(), count: 1, avatar: String::new()
        };
        let mut catalog = vec![group("g", "Fixture"), group("a", "AA-PLY001-X - jx")];
        let changed = app.forwarding_changed.subscribe();
        app.set_groups(catalog.clone()).await.unwrap();
        assert!(changed.has_changed().unwrap()); // The bridge must reconfigure its native download allowlist.
        assert_eq!(*app.forwarding.read().await, BTreeSet::from(["g".into(), "a".into()]));
        let groups = json_body(call("GET", "/api/groups", token, json!({})).await.unwrap()).await;
        assert_eq!(groups.as_array().unwrap().len(), 1);
        assert_eq!(groups[0]["id"], "a");
        let explicit = json_body(call("GET", "/api/groups", "fixture-independent-secret", json!({})).await.unwrap()).await;
        assert_eq!(explicit.as_array().unwrap().len(), 1);
        assert_eq!(explicit[0]["id"], "g");
        let status = json_body(call("GET", "/api/status", token, json!({})).await.unwrap()).await;
        assert_eq!(status["permissions"]["groups"], json!(["a"]));
        assert_eq!(status["permissions"]["can_send"], false);
        assert_eq!(call("POST", "/api/messages", token,
            json!({"id":uuid::Uuid::new_v4().to_string(),"group":"a","client_id":"fixture-client","sender":"Fixture","text":"fixture"})).await.unwrap().status(), StatusCode::FORBIDDEN);

        let source = temp.path().join("media/fixture.bin");
        tokio::fs::write(&source, b"fixture").await.unwrap();
        app.store.set_media(&Media { id:"rule-media".into(), name:"fixture.bin".into(), kind:"file".into(),
            size:7, sha256:"unused-by-download".into(), path:source.to_string_lossy().into() }, "a").unwrap();
        assert_eq!(call("GET", "/api/media/rule-media", token, json!({})).await.unwrap().status(), StatusCode::OK);
        assert_eq!(call("GET", "/api/media/rule-media", "fixture-independent-secret", json!({})).await.unwrap().status(), StatusCode::FORBIDDEN);

        catalog.push(group("b", "BB-PLY002 - jx"));
        catalog.push(group("unrelated", "MH-OTHER - jx"));
        app.set_groups(catalog.clone()).await.unwrap();
        let groups = json_body(call("GET", "/api/groups", token, json!({})).await.unwrap()).await;
        assert_eq!(groups.as_array().unwrap().len(), 2);
        let principal = Principal::Client { id:client_id.into(), revision:1 };
        assert!(visible_event(&app, &principal, &json!({"type":"message","message":{"group":"b"}})).await);
        assert!(!visible_event(&app, &principal, &json!({"type":"message","message":{"group":"unrelated"}})).await);

        catalog[1].name = "Renamed ordinary group".into();
        app.set_groups(catalog).await.unwrap();
        assert_eq!(*app.forwarding.read().await, BTreeSet::from(["g".into(), "b".into()]));
        assert_eq!(call("GET", "/api/media/rule-media", token, json!({})).await.unwrap().status(), StatusCode::FORBIDDEN);
        assert!(!visible_event(&app, &principal, &json!({"type":"message","message":{"group":"a"}})).await);
        let groups = json_body(call("GET", "/api/groups", token, json!({})).await.unwrap()).await;
        assert_eq!(groups.as_array().unwrap().len(), 1);
        assert_eq!(groups[0]["id"], "b");
        let persisted = crate::store::Store::open(&temp.path().join("test.sqlite")).unwrap();
        assert_eq!(persisted.forward_patterns().unwrap(), BTreeSet::from([pattern.into()]));
        assert_eq!(persisted.effective_forward_groups().unwrap(), *app.forwarding.read().await);
        assert!(persisted.clients().unwrap()[client_id].allows_group("b"));
        assert!(!persisted.clients().unwrap()[client_id].allows_group("a"));
        // An older administrator may update manual choices without erasing rules.
        let response = call("POST", "/api/local/forwarding", &app.config.token, json!({"selected":["g"]})).await.unwrap();
        let snapshot = json_body(response).await;
        assert_eq!(snapshot["manual_selected"], json!(["g"]));
        assert_eq!(snapshot["patterns"], json!([pattern]));
        assert_eq!(snapshot["selected"], json!(["b","g"]));
        let response = call("POST", &format!("/api/local/clients/{client_id}"), &app.config.token,
            json!({"group_patterns":[]})).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert!(json_body(call("GET", "/api/groups", token, json!({})).await.unwrap()).await.as_array().unwrap().is_empty());
        assert!(app.allows("b").await); // Client rules never alter the global selection.
        let response = call("POST", "/api/local/forwarding", &app.config.token,
            json!({"selected":["g"],"patterns":[]})).await.unwrap();
        assert_eq!(json_body(response).await["selected"], json!(["g"]));
        let response = call("POST", "/api/local/forwarding", &app.config.token,
            json!({"selected":["g"],"patterns":["invalid\nrule"]})).await.unwrap();
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
        assert!(app.store.forward_patterns().unwrap().is_empty());
    }

    #[tokio::test]
    async fn old_image_reference_is_refreshed_from_the_stored_reply_before_locating() {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut commands) = crate::test_app(temp.path());
        grant(&app).await;
        app.status.write().await["ready"] = json!(true);
        let mut original: ChatMessage = serde_json::from_value(json!({"id":"original-image","group":"g","sender":"成员","text":"[图片]","timestamp":1000,"status":"received"})).unwrap();
        app.store.insert_message(&mut original).unwrap();
        let source = "wx:g:message__message_0.db:7:70";
        let mut reply: ChatMessage = serde_json::from_value(json!({"id":"old-reply","group":"g","source_id":source,"sender":"成员","text":"这个","timestamp":2000,"status":"received","quote":{"id":"unavailable-42","sender":"成员","text":"[图片]"}})).unwrap();
        app.store.insert_message(&mut reply).unwrap();
        let router = client_router(app.clone());
        let response = tokio::spawn({
            let router = router.clone();
            async move { router.oneshot(request("POST", "/api/messages/old-reply/quote-source?group=g", "fixture-independent-secret", "192.0.2.3:2345", Body::from(r#"{"source_id":"not-trusted"}"#))).await.unwrap() }
        });
        let job = tokio::time::timeout(Duration::from_secs(1), commands.recv()).await.expect("legacy references must request their verified source").unwrap();
        assert_eq!(job["op"], "quote_refresh");
        assert_eq!(job["source_id"], source);
        assert_eq!(job["_access_client"], "fixture-key");
        reply.quote.as_mut().unwrap().id = original.id.clone();
        app.store.enrich_native_quote(&reply.id, &reply.text, reply.quote.as_ref().unwrap(), false, Some("42")).unwrap();
        app.publish(json!({"type":"quote_refresh_done","group":"g","source_id":source,"request_id":job["request_id"],"ok":true}));
        let response = response.await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let value = json_body(response).await;
        assert_eq!(value["message"]["quote"]["id"], original.id);
        assert_eq!(value["original"]["id"], original.id);
        app.status.write().await["ready"] = json!(false);
        let cached = router.oneshot(request("POST", "/api/messages/old-reply/quote-source?group=g", "fixture-independent-secret", "192.0.2.3:2345", Body::empty())).await.unwrap();
        assert_eq!(cached.status(), StatusCode::OK);
        assert!(commands.try_recv().is_err());
        app.status.write().await["ready"] = json!(true);
        reply.id = "revoked-reply".into();
        reply.source_id = "wx:g:message__message_0.db:8:80".into();
        reply.quote.as_mut().unwrap().id = "unavailable-43".into();
        app.store.insert_message(&mut reply).unwrap();
        let denied = tokio::spawn({
            let router = client_router(app.clone());
            async move { router.oneshot(request("POST", "/api/messages/revoked-reply/quote-source?group=g", "fixture-independent-secret", "192.0.2.3:2345", Body::empty())).await.unwrap() }
        });
        let job = tokio::time::timeout(Duration::from_secs(1), commands.recv()).await.unwrap().unwrap();
        app.clients.write().await.get_mut("fixture-key").unwrap().access.enabled = false;
        app.publish(json!({"type":"quote_refresh_done","group":"g","source_id":job["source_id"],"request_id":job["request_id"],"ok":true}));
        assert_eq!(tokio::time::timeout(Duration::from_secs(1), denied).await.unwrap().unwrap().status(), StatusCode::FORBIDDEN);
    }

    #[tokio::test]
    async fn unsupported_background_recall_is_rejected_before_state_change_or_enqueue() {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut receiver) = crate::test_app(temp.path());
        app.set_status_recall(
            true,
            "fixture",
            "",
            Some(false),
            crate::model::RECALL_UNAVAILABLE,
        )
        .await;
        let mut message:ChatMessage=serde_json::from_value(json!({"id":"unsupported","group":"g","sender":"Fixture","text":"original body","timestamp":chrono::Utc::now().timestamp_millis(),"status":"sent","outgoing":true,"source_id":"native-source"})).unwrap();
        app.store.insert_message(&mut message).unwrap();
        let router = super::router(app.clone());
        for retryable in [false, true] {
            if retryable {
                let mut pending = app
                    .store
                    .begin_recall(&message.id, message.timestamp)
                    .unwrap()
                    .0;
                app.store
                    .finish_recall(
                        &message.id,
                        &pending.recall_attempt,
                        "failed",
                        "proved not submitted",
                        true,
                    )
                    .unwrap();
                pending = app.store.message(&message.id).unwrap().unwrap();
                assert!(pending.recall_retryable);
            }
            let before = app.store.message(&message.id).unwrap().unwrap();
            let response = router
                .clone()
                .oneshot(request(
                    "POST",
                    "/api/messages/unsupported/recall",
                    &app.config.token,
                    "127.0.0.1:2345",
                    Body::empty(),
                ))
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::NOT_IMPLEMENTED);
            assert_eq!(
                json_body(response).await["error"],
                crate::model::RECALL_UNAVAILABLE
            );
            assert_eq!(
                serde_json::to_value(app.store.message(&message.id).unwrap().unwrap()).unwrap(),
                serde_json::to_value(before).unwrap()
            );
            assert!(receiver.try_recv().is_err());
        }
        let status = principal_status(&app, &Principal::Admin).await;
        assert_eq!(status["recall_supported"], false);
        assert_eq!(status["recall_reason"], crate::model::RECALL_UNAVAILABLE);
    }

    fn request(method: &str, uri: &str, token: &str, peer: &str, body: Body) -> Request<Body> {
        let mut request = Request::builder()
            .method(method)
            .uri(uri)
            .header(header::AUTHORIZATION, format!("Bearer {token}"))
            .header(header::CONTENT_TYPE, "application/json")
            .body(body)
            .unwrap();
        request
            .extensions_mut()
            .insert(ConnectInfo(peer.parse::<SocketAddr>().unwrap()));
        request
    }

    async fn json_body(response: Response) -> Value {
        serde_json::from_slice(
            &axum::body::to_bytes(response.into_body(), 64 * 1024)
                .await
                .unwrap(),
        )
        .unwrap()
    }

    fn access_record() -> ClientRecord {
        ClientRecord {
            access: ClientAccess {
                id: "fixture-key".into(),
                name: "Fixture client".into(),
                enabled: true,
                revoked: false,
                groups: BTreeSet::from(["g".into()]),
                group_patterns: BTreeSet::new(),
                can_send: true,
                can_download: true,
                created_at: 1,
                updated_at: 1,
                revision: 1,
            },
            token_hash: access::token_hash("fixture-independent-secret"),
            token: None,
            matched_groups: BTreeSet::new(),
        }
    }

    #[tokio::test]
    async fn video_poster_uses_a_small_cache_and_enforces_download_and_group_permissions() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        let original = temp.path().join("media/offline.mp4");
        tokio::fs::write(&original, b"original-is-not-a-thumbnail")
            .await
            .unwrap();
        let media = Media {
            id: "poster-fixture".into(),
            name: "offline.mp4".into(),
            kind: "video".into(),
            size: 27,
            sha256: "a".repeat(64),
            path: original.to_string_lossy().into_owned(),
        };
        app.store.set_media(&media, "g").unwrap();
        let cache = temp.path().join("video-posters");
        tokio::fs::create_dir(&cache).await.unwrap();
        let key = format!(
            "{:x}",
            Sha256::digest(format!("{}:{}", media.id, media.sha256).as_bytes())
        );
        let cover = vec![
            0xff, 0xd8, 0xff, 0xc0, 0, 8, 8, 0, 90, 0, 160, 1, 0xff, 0xd9,
        ];
        tokio::fs::write(cache.join(format!("{key}.jpg")), &cover)
            .await
            .unwrap();
        let router = client_router(app.clone());
        let get = || {
            request(
                "GET",
                "/api/media/poster-fixture/poster",
                "fixture-independent-secret",
                "192.0.2.3:2345",
                Body::empty(),
            )
        };
        let response = router.clone().oneshot(get()).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CONTENT_TYPE], "image/jpeg");
        assert_eq!(
            response.headers()["X-Content-SHA256"],
            format!("{:x}", Sha256::digest(&cover))
        );
        assert_eq!(
            axum::body::to_bytes(response.into_body(), 48 * 1024)
                .await
                .unwrap()
                .as_ref(),
            &cover
        );
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .can_download = false;
        assert_eq!(
            router.clone().oneshot(get()).await.unwrap().status(),
            StatusCode::FORBIDDEN
        );
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .can_download = true;
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .groups
            .clear();
        assert_eq!(
            router.clone().oneshot(get()).await.unwrap().status(),
            StatusCode::FORBIDDEN
        );
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .groups
            .insert("g".into());
        app.forwarding.write().await.clear();
        assert_eq!(
            router.oneshot(get()).await.unwrap().status(),
            StatusCode::FORBIDDEN
        );
    }

    async fn grant(app: &App) -> Principal {
        let record = access_record();
        app.store.save_client(&record, true).unwrap();
        app.clients
            .write()
            .await
            .insert(record.access.id.clone(), record);
        Principal::Client {
            id: "fixture-key".into(),
            revision: 1,
        }
    }

    #[tokio::test]
    async fn records_http_empty_filters_advance_and_readonly_keys_cannot_cross_groups_or_change_cursor_filter()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        {
            let mut clients = app.clients.write().await;
            let access = &mut clients.get_mut("fixture-key").unwrap().access;
            access.can_send = false;
            access.can_download = false;
        }
        app.store
            .set_history_progress(
                "g",
                &crate::history::Progress {
                    exhausted: true,
                    ..Default::default()
                },
            )
            .unwrap();
        for index in 1..=230 {
            let mut row:ChatMessage=serde_json::from_value(json!({"id":format!("record-{index}"),"group":"g","sender":"成员","text":if index==1 {"needle"} else {"ordinary"},"timestamp":1000,"status":"received"})).unwrap();
            app.store.insert_message(&mut row).unwrap();
        }
        let router = client_router(app.clone());
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                "/api/records?group=g&q=needle",
                "fixture-independent-secret",
                "192.0.2.3:2345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let first = json_body(response).await;
        assert_eq!(first["messages"], json!([]));
        assert_eq!(first["has_more"], true);
        let cursor = first["next_before"].as_str().unwrap();
        for path in [
            format!("/api/records?group=g&q=changed&before={cursor}"),
            "/api/records?group=g&type=invalid".into(),
            "/api/records?group=g&start=2&end=1".into(),
            "/api/records?group=g&before=ff".into(),
        ] {
            assert_eq!(
                router
                    .clone()
                    .oneshot(request(
                        "GET",
                        &path,
                        "fixture-independent-secret",
                        "192.0.2.3:2345",
                        Body::empty()
                    ))
                    .await
                    .unwrap()
                    .status(),
                StatusCode::BAD_REQUEST
            );
        }
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                &format!("/api/records?group=g&q=needle&before={cursor}"),
                "fixture-independent-secret",
                "192.0.2.3:2345",
                Body::empty(),
            ))
            .await
            .unwrap();
        let last = json_body(response).await;
        assert_eq!(last["messages"][0]["id"], "record-1");
        assert_eq!(last["next_before"], Value::Null);
        app.forwarding.write().await.insert("other".into());
        for path in ["/api/records?group=other", "/api/groups/other/announcement"] {
            assert_eq!(
                router
                    .clone()
                    .oneshot(request(
                        "GET",
                        path,
                        "fixture-independent-secret",
                        "192.0.2.3:2345",
                        Body::empty()
                    ))
                    .await
                    .unwrap()
                    .status(),
                StatusCode::FORBIDDEN
            );
        }
    }

    #[tokio::test]
    async fn records_native_pages_keep_the_tail_and_progress_on_empty_native_matches_without_false_eof()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut commands) = crate::test_app(temp.path());
        grant(&app).await;
        app.status.write().await["ready"] = json!(true);
        let router = client_router(app.clone());
        let first = tokio::spawn({
            let router = router.clone();
            async move {
                router
                    .oneshot(request(
                        "GET",
                        "/api/records?group=g",
                        "fixture-independent-secret",
                        "192.0.2.3:2345",
                        Body::empty(),
                    ))
                    .await
                    .unwrap()
            }
        });
        let job = commands.recv().await.unwrap();
        assert_eq!(job["op"], "history");
        let mut sources = Vec::new();
        for index in 1..=60 {
            let source = format!("wx:g:message__message_0.db:{index}:{index}");
            sources.push(source.clone());
            let mut row:ChatMessage=serde_json::from_value(json!({"id":format!("row-{index}"),"source_id":source,"group":"g","sender":"成员","text":"ordinary","timestamp":1000,"status":"received"})).unwrap();
            app.store.insert_historical_message(&mut row).unwrap();
        }
        app.history.finish(&app,json!({"group":"g","request_id":job["request_id"],"before_source":sources[0],"has_more":true,"sources":sources})).await.unwrap();
        let first = json_body(first.await.unwrap()).await;
        assert_eq!(first["messages"].as_array().unwrap().len(), 50);
        let cursor = first["next_before"].as_str().unwrap();
        let tail = router
            .clone()
            .oneshot(request(
                "GET",
                &format!("/api/records?group=g&before={cursor}"),
                "fixture-independent-secret",
                "192.0.2.3:2345",
                Body::empty(),
            ))
            .await
            .unwrap();
        let tail = json_body(tail).await;
        assert_eq!(tail["messages"].as_array().unwrap().len(), 10);
        assert_eq!(tail["messages"][0]["id"], "row-10");
        assert!(commands.try_recv().is_err());
        let mut before = tail["next_before"].as_str().unwrap().to_owned();
        // Two unsupported/duplicate-only native pages must still move their opaque cursor.
        for (position, more) in [(0, true), (-1, false)] {
            let task = tokio::spawn({
                let router = router.clone();
                let before = before.clone();
                async move {
                    router
                        .oneshot(request(
                            "GET",
                            &format!("/api/records?group=g&before={before}"),
                            "fixture-independent-secret",
                            "192.0.2.3:2345",
                            Body::empty(),
                        ))
                        .await
                        .unwrap()
                }
            });
            let job = commands.recv().await.unwrap();
            app.history.finish(&app,json!({"group":"g","request_id":job["request_id"],"before_source":format!("wx:g:message__message_0.db:0:{position}"),"has_more":more,"sources":[]})).await.unwrap();
            let value = json_body(task.await.unwrap()).await;
            assert_eq!(value["messages"], json!([]));
            assert_eq!(value["pending"], false);
            assert_eq!(value["has_more"], more);
            if more {
                let next = value["next_before"].as_str().unwrap().to_owned();
                assert_ne!(before, next);
                before = next;
            } else {
                assert_eq!(value["next_before"], Value::Null);
            }
        }
    }

    #[tokio::test]
    async fn announcement_http_distinguishes_unset_and_rechecks_client_permission_after_native_wait()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut commands) = crate::test_app(temp.path());
        grant(&app).await;
        app.status.write().await["ready"] = json!(true);
        let router = client_router(app.clone());
        for revoke in [false, true] {
            let task = tokio::spawn({
                let router = router.clone();
                async move {
                    router
                        .oneshot(request(
                            "GET",
                            "/api/groups/g/announcement",
                            "fixture-independent-secret",
                            "192.0.2.3:2345",
                            Body::empty(),
                        ))
                        .await
                        .unwrap()
                }
            });
            let command = commands.recv().await.unwrap();
            assert_eq!(command["op"], "announcement");
            assert_eq!(command["group"], "g");
            if revoke {
                app.clients
                    .write()
                    .await
                    .get_mut("fixture-key")
                    .unwrap()
                    .access
                    .groups
                    .clear();
            }
            app.announcements.finish(json!({"group":"g","request_id":command["request_id"],"available":true,"set":false,"text":"","author_id":"","author":"","avatar":"","updated_at":null})).await;
            let response = task.await.unwrap();
            if revoke {
                assert_eq!(response.status(), StatusCode::FORBIDDEN);
            } else {
                let value = json_body(response).await;
                assert_eq!(value["available"], true);
                assert_eq!(value["set"], false);
                assert_eq!(value["updated_at"], Value::Null);
            }
        }
    }

    #[tokio::test]
    async fn announcement_http_timeout_is_bounded_unavailable_and_retries_use_a_new_generation() {
        let temp = tempfile::tempdir().unwrap();
        let (app, mut commands) = crate::test_app(temp.path());
        grant(&app).await;
        app.status.write().await["ready"] = json!(true);
        let response = tokio::time::timeout(
            Duration::from_secs(4),
            client_router(app.clone()).oneshot(request(
                "GET",
                "/api/groups/g/announcement",
                "fixture-independent-secret",
                "192.0.2.3:2345",
                Body::empty(),
            )),
        )
        .await
        .unwrap()
        .unwrap();
        let first = commands.recv().await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let value = json_body(response).await;
        assert_eq!(value["available"], false);
        assert_eq!(value["pending"], true);
        assert_eq!(value["text"], "");
        let (new_id, new_reply) = app.announcements.request(&app, "g", "").await.unwrap();
        assert_ne!(first["request_id"], new_id);
        app.announcements
            .finish(
                json!({"group":"g","request_id":first["request_id"],"available":true,"set":false}),
            )
            .await;
        assert!(new_reply.borrow().is_none());
        app.announcements.fail_all().await;
    }

    #[tokio::test]
    async fn client_entry_rejects_administrator_and_does_not_mount_management_routes() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        let router = client_router(app.clone());
        for path in ["/api/status", "/api/groups", "/api/events"] {
            let mut request = request(
                "GET",
                path,
                &app.config.token,
                "127.0.0.1:12345",
                Body::empty(),
            );
            for name in ["x-forwarded-for", "x-real-ip", "x-wechat-entry-point"] {
                request
                    .headers_mut()
                    .insert(name, "127.0.0.1".parse().unwrap());
            }
            let response = router.clone().oneshot(request).await.unwrap();
            assert_eq!(response.status(), StatusCode::FORBIDDEN, "{path}");
        }
        for (method, path) in [
            ("GET", "/api/local/p2p"),
            ("GET", "/api/local/clients"),
            ("POST", "/api/local/clients"),
            ("POST", "/api/local/clients/fixture-key"),
            ("POST", "/api/local/clients/fixture-key/revoke"),
            ("GET", "/api/local/clients/fixture-key/credential"),
            ("POST", "/api/local/clients/fixture-key/rotate"),
            ("GET", "/api/local/forwarding"),
            ("POST", "/api/local/forwarding"),
            ("POST", "/api/shutdown"),
        ] {
            let response = router
                .clone()
                .oneshot(request(
                    method,
                    path,
                    "fixture-independent-secret",
                    "127.0.0.1:12345",
                    Body::from("{}"),
                ))
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::NOT_FOUND, "{method} {path}");
        }
        assert!(!*app.shutdown.borrow());
        let response = router
            .oneshot(request(
                "GET",
                "/api/status",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let status = json_body(response).await;
        assert_eq!(status["authenticated_client"]["admin"], false);
        assert_eq!(status["permissions"]["groups"], json!(["g"]));
        assert!(status.get("online_clients").is_none());
    }

    #[tokio::test]
    async fn credentials_require_real_loopback_admin_and_never_leak_through_policy_responses() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        let router = router(app.clone());
        for (method, path) in [
            ("GET", "/api/local/clients/fixture-key/credential"),
            ("POST", "/api/local/clients/fixture-key/rotate"),
        ] {
            for (token, peer) in [
                ("fixture-independent-secret", "127.0.0.1:12345"),
                (app.config.token.as_str(), "192.0.2.3:12345"),
            ] {
                let mut denied = request(method, path, token, peer, Body::from("{}"));
                denied
                    .headers_mut()
                    .insert("x-forwarded-for", "127.0.0.1".parse().unwrap());
                denied
                    .headers_mut()
                    .insert("x-admin", "true".parse().unwrap());
                assert_eq!(
                    router.clone().oneshot(denied).await.unwrap().status(),
                    StatusCode::FORBIDDEN
                );
            }
        }
        let secret = "fixture-independent-secret";
        let hash = access::token_hash(secret);
        for (path, token) in [
            ("/api/local/clients", app.config.token.as_str()),
            ("/api/status", app.config.token.as_str()),
            ("/api/status", secret),
        ] {
            let response = router
                .clone()
                .oneshot(request(
                    "GET",
                    path,
                    token,
                    "127.0.0.1:12345",
                    Body::empty(),
                ))
                .await
                .unwrap();
            assert_eq!(response.status(), StatusCode::OK);
            let text = json_body(response).await.to_string();
            assert!(!text.contains(secret) && !text.contains(&hash) && !text.contains("token"));
        }
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                "/api/local/clients/fixture-key/credential",
                &app.config.token,
                "[::1]:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CACHE_CONTROL], "no-store");
        assert_eq!(json_body(response).await["token"], secret);
        let response = router
            .oneshot(request(
                "GET",
                "/api/local/clients/missing/credential",
                &app.config.token,
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
    }

    #[tokio::test]
    async fn legacy_auth_learns_once_without_revision_change_or_accepting_rejected_tokens() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        assert!(app.store.clients().unwrap()["fixture-key"].token.is_none());
        assert!(
            authenticate(&app, "not-the-client-token", None)
                .await
                .is_err()
        );
        assert!(app.store.clients().unwrap()["fixture-key"].token.is_none());
        {
            let mut clients = app.clients.write().await;
            let record = clients.get_mut("fixture-key").unwrap();
            record.access.enabled = false;
            app.store.save_client(record, false).unwrap();
        }
        assert!(
            authenticate(&app, "fixture-independent-secret", None)
                .await
                .is_err()
        );
        assert!(app.store.clients().unwrap()["fixture-key"].token.is_none());
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .enabled = true;
        app.store.save_client(&access_record(), false).unwrap();
        let principal = authenticate(&app, "fixture-independent-secret", None)
            .await
            .unwrap();
        assert!(matches!(principal, Principal::Client { revision: 1, .. }));
        assert_eq!(
            app.store.clients().unwrap()["fixture-key"].token.as_deref(),
            Some("fixture-independent-secret")
        );
        assert_eq!(app.clients.read().await["fixture-key"].access.revision, 1);
        // A repeated valid request must take the in-memory path, with no secret-table write.
        let audit = rusqlite::Connection::open(temp.path().join("test.sqlite")).unwrap();
        audit.execute_batch("CREATE TRIGGER reject_repeated_secret_insert BEFORE INSERT ON access_client_secrets BEGIN SELECT RAISE(ABORT,'unexpected repeated secret write'); END;
            CREATE TRIGGER reject_repeated_secret_update BEFORE UPDATE ON access_client_secrets BEGIN SELECT RAISE(ABORT,'unexpected repeated secret write'); END;").unwrap();
        assert!(
            authenticate(&app, "fixture-independent-secret", None)
                .await
                .is_ok()
        );
    }

    #[tokio::test]
    async fn racing_legacy_learning_with_rotation_or_revocation_cannot_restore_the_old_token() {
        for (operation, auth_first) in [
            ("rotate", true),
            ("rotate", false),
            ("revoke", true),
            ("revoke", false),
        ] {
            let temp = tempfile::tempdir().unwrap();
            let (app, _receiver) = crate::test_app(temp.path());
            grant(&app).await;
            let auth = authenticate(&app, "fixture-independent-secret", None);
            let change = router(app.clone()).oneshot(request(
                "POST",
                &format!("/api/local/clients/fixture-key/{operation}"),
                &app.config.token,
                "127.0.0.1:12345",
                Body::from("{}"),
            ));
            let response = if auth_first {
                tokio::join!(auth, change).1.unwrap()
            } else {
                tokio::join!(change, auth).0.unwrap()
            };
            assert_eq!(response.status(), StatusCode::OK);
            let changed = json_body(response).await;
            assert!(
                authenticate(&app, "fixture-independent-secret", None)
                    .await
                    .is_err()
            );
            let persisted = app.store.clients().unwrap();
            let current = &persisted["fixture-key"];
            if operation == "rotate" {
                assert_eq!(current.token.as_deref(), changed["token"].as_str());
                assert_ne!(current.token.as_deref(), Some("fixture-independent-secret"));
                assert!(
                    authenticate(&app, current.token.as_deref().unwrap(), None)
                        .await
                        .is_ok()
                );
            } else {
                assert!(current.token.is_none() && current.access.revoked);
            }
        }
    }

    #[tokio::test]
    async fn client_entry_preserves_group_and_attachment_permissions_and_revocation() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        let router = client_router(app.clone());
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                "/api/groups",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(json_body(response).await[0]["id"], "g");

        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .groups
            .clear();
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                "/api/messages?group=g",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::FORBIDDEN);
        {
            let mut clients = app.clients.write().await;
            let access = &mut clients.get_mut("fixture-key").unwrap().access;
            access.groups.insert("g".into());
            access.can_send = false;
            access.can_download = false;
        }
        let response = router
            .clone()
            .oneshot(request(
                "POST",
                "/api/messages",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::from(
                    json!({
                        "id":uuid::Uuid::new_v4().to_string(),"group":"g",
                        "client_id":"","sender":"Fixture","text":"fixture"
                    })
                    .to_string(),
                ),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::FORBIDDEN);
        let response = router
            .clone()
            .oneshot(request(
                "POST",
                "/api/media?group=g&name=fixture.bin",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::from("fixture"),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::FORBIDDEN);
        app.store
            .set_media(
                &Media {
                    id: "fixture-media".into(),
                    name: "fixture.bin".into(),
                    kind: "file".into(),
                    size: 7,
                    sha256: String::new(),
                    path: temp
                        .path()
                        .join("media/fixture.bin")
                        .to_string_lossy()
                        .into_owned(),
                },
                "g",
            )
            .unwrap();
        let response = router
            .clone()
            .oneshot(request(
                "GET",
                "/api/media/fixture-media",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::FORBIDDEN);
        app.clients
            .write()
            .await
            .get_mut("fixture-key")
            .unwrap()
            .access
            .revoked = true;
        let response = router
            .oneshot(request(
                "GET",
                "/api/status",
                "fixture-independent-secret",
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
        assert_eq!(app.transfers.available_permits(), 3);
    }

    #[tokio::test]
    async fn p2p_metadata_requires_real_local_administrator_and_excludes_credentials() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        grant(&app).await;
        *app.p2p_status.write().await = json!({
            "enabled":true,"server_id":"fixture-public-id","status":"listening",
            "relay_token":"must-not-leak","relay_ca":"private-config","private_key":"must-not-leak"
        });
        let router = router(app.clone());
        for (token, peer) in [
            (app.config.token.as_str(), "192.0.2.3:12345"),
            ("fixture-independent-secret", "127.0.0.1:12345"),
        ] {
            let mut request = request("GET", "/api/local/p2p", token, peer, Body::empty());
            request
                .headers_mut()
                .insert("x-forwarded-for", "127.0.0.1".parse().unwrap());
            request
                .headers_mut()
                .insert("forwarded", "for=127.0.0.1".parse().unwrap());
            assert_eq!(
                router.clone().oneshot(request).await.unwrap().status(),
                StatusCode::FORBIDDEN
            );
        }
        let response = router
            .oneshot(request(
                "GET",
                "/api/local/p2p",
                &app.config.token,
                "127.0.0.1:12345",
                Body::empty(),
            ))
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(
            json_body(response).await,
            json!({
                "enabled":true,"server_id":"fixture-public-id","status":"listening"
            })
        );
    }

    #[tokio::test]
    async fn administrator_secret_cannot_authenticate_remote_or_unknown_peers_and_client_cannot_administer()
     {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        let principal = grant(&app).await;
        for peer in [None, Some("192.0.2.3:4567".parse().unwrap())] {
            assert_eq!(
                authenticate(&app, &app.config.token, peer)
                    .await
                    .unwrap_err()
                    .1,
                StatusCode::FORBIDDEN
            );
        }
        assert!(matches!(
            authenticate(&app, &app.config.token, Some("[::1]:4567".parse().unwrap()))
                .await
                .unwrap(),
            Principal::Admin
        ));
        assert!(matches!(
            authenticate(
                &app,
                "fixture-independent-secret",
                Some("192.0.2.3:4567".parse().unwrap())
            )
            .await
            .unwrap(),
            Principal::Client { .. }
        ));
        assert_eq!(
            check_admin(&principal, "127.0.0.1:4567".parse().unwrap())
                .unwrap_err()
                .1,
            StatusCode::FORBIDDEN
        );
        let mut changed = app.clients.write().await;
        changed.get_mut("fixture-key").unwrap().access.enabled = false;
        drop(changed);
        assert_eq!(
            authenticate(
                &app,
                "fixture-independent-secret",
                Some("127.0.0.1:4567".parse().unwrap())
            )
            .await
            .unwrap_err()
            .1,
            StatusCode::UNAUTHORIZED
        );
        assert_eq!(
            public(
                json!({"message":{"access_client":"secret-internal","recall_client":"actor","media":[{"path":"local-private"}]}})
            ),
            json!({"message":{"media":[{}]}})
        );
    }

    #[tokio::test]
    async fn attachment_download_stops_between_chunks_when_permission_changes() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        let principal = grant(&app).await;
        let path = temp.path().join("media").join("large.bin");
        let bytes = vec![42; 200_000];
        std::fs::write(&path, &bytes).unwrap();
        app.store
            .set_media(
                &Media {
                    id: "attachment".into(),
                    name: "large.bin".into(),
                    kind: "file".into(),
                    size: bytes.len() as u64,
                    sha256: String::new(),
                    path: path.to_string_lossy().into_owned(),
                },
                "g",
            )
            .unwrap();
        let response = download(
            State(app.clone()),
            Extension(principal),
            Path("attachment".into()),
        )
        .await
        .unwrap();
        let mut stream = response.into_body().into_data_stream();
        let first = stream.next().await.unwrap().unwrap();
        assert!(!first.is_empty() && first.len() < bytes.len());
        {
            let mut clients = app.clients.write().await;
            let record = clients.get_mut("fixture-key").unwrap();
            record.access.can_download = false;
            record.access.revision += 1;
            app.store.save_client(record, false).unwrap();
            app.clients_changed.send_replace(());
        }
        assert!(stream.next().await.is_none());
        drop(stream);
        assert_eq!(app.transfers.available_permits(), 3);
    }

    #[tokio::test]
    async fn attachment_upload_revocation_discards_partial_file_and_media_record() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        let principal = grant(&app).await;
        let live = app.clone();
        let body = Body::from_stream(futures_util::stream::unfold(
            (0, live),
            |(step, app)| async move {
                if step >= 2 {
                    return None;
                }
                if step == 1 {
                    let mut clients = app.clients.write().await;
                    let record = clients.get_mut("fixture-key").unwrap();
                    record.access.can_send = false;
                    record.access.revision += 1;
                    app.store.save_client(record, false).unwrap();
                    app.clients_changed.send_replace(());
                }
                Some((Ok::<_, std::io::Error>(vec![7u8; 16]), (step + 1, app)))
            },
        ));
        let mut headers = HeaderMap::new();
        headers.insert(header::CONTENT_LENGTH, "32".parse().unwrap());
        let error = upload(
            State(app.clone()),
            Extension(principal),
            Query(UploadQuery {
                name: "upload.bin".into(),
                group: "g".into(),
            }),
            headers,
            body,
        )
        .await
        .unwrap_err();
        assert_eq!(error.1, StatusCode::FORBIDDEN);
        for day in std::fs::read_dir(temp.path().join("media")).unwrap() {
            assert_eq!(std::fs::read_dir(day.unwrap().path()).unwrap().count(), 0);
        }
        let conn = rusqlite::Connection::open(temp.path().join("test.sqlite")).unwrap();
        assert_eq!(
            conn.query_row("SELECT count(*) FROM media", [], |row| row.get::<_, i64>(0))
                .unwrap(),
            0
        );
    }

    #[tokio::test]
    async fn management_endpoint_accepts_only_loopback_peers() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        let response = shutdown(
            State(app.clone()),
            Extension(Principal::Admin),
            ConnectInfo("192.0.2.1:1234".parse().unwrap()),
        )
        .await
        .into_response();
        assert_eq!(response.status(), StatusCode::FORBIDDEN);
        assert!(!*app.shutdown.borrow());
        let response = shutdown(
            State(app.clone()),
            Extension(Principal::Admin),
            ConnectInfo("[::1]:1234".parse().unwrap()),
        )
        .await
        .into_response();
        assert_eq!(response.status(), StatusCode::OK);
        assert!(*app.shutdown.borrow());
    }

    #[tokio::test]
    async fn forwarding_management_rejects_remote_peers_and_events_obey_current_selection() {
        let temp = tempfile::tempdir().unwrap();
        let (app, _receiver) = crate::test_app(temp.path());
        let remote = ConnectInfo("192.0.2.1:1234".parse().unwrap());
        assert_eq!(
            local_forwarding(State(app.clone()), Extension(Principal::Admin), remote)
                .await
                .err()
                .unwrap()
                .into_response()
                .status(),
            StatusCode::FORBIDDEN
        );
        assert_eq!(
            set_forwarding(
                State(app.clone()),
                Extension(Principal::Admin),
                remote,
                Json(ForwardingRequest { selected: vec![], patterns: None })
            )
            .await
            .err()
            .unwrap()
            .into_response()
            .status(),
            StatusCode::FORBIDDEN
        );
        assert!(app.allows("g").await);
        let visible = json!({"type":"message","message":{"group":"g","text":"allowed"}});
        assert!(visible_event(&app, &Principal::Admin, &visible).await);
        assert!(
            !visible_event(
                &app,
                &Principal::Admin,
                &json!({"type":"message","message":{"group":"hidden","text":"secret"}})
            )
            .await
        );
        app.set_forwarding(BTreeSet::new()).await.unwrap();
        assert!(!visible_event(&app, &Principal::Admin, &visible).await);
        assert!(
            !visible_event(
                &app,
                &Principal::Admin,
                &json!({"type":"members","group":"g"})
            )
            .await
        );
    }
}
