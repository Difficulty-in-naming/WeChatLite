//! Android JNI entry points for the P2P tunnel.
//!
//! Android cannot execute a bundled ELF from app storage: `untrusted_app` may not run files under
//! `app_data_file`, and packaging the binary under `lib/` makes the loader treat it as a shared
//! library, aborting with `android context was not initialized`. The supported shape is an
//! in-process cdylib: Kotlin calls these functions through JNI and the tunnel runs on its own tokio
//! runtime inside the app process.
//!
//! The desktop helper keeps using `main.rs` with stdin/stdout; both paths share `bind_client` and
//! `client`, so the tunnelling behaviour is identical.
//!
//! iroh reads the system DNS servers through JNI on Android, using the `ndk_context` crate. That
//! context is normally installed by `ndk-glue`/`android-activity` before `main`; an app embedding
//! the library has neither, so Kotlin must hand us the `JavaVM` and application `Context` through
//! [`Java_local_wechatlite_mobile_net_TunnelProcess_nativeInstallContext`] before any endpoint is
//! built. The release profile is `panic = "abort"`, so iroh cannot fall back gracefully when the
//! context is missing: it aborts the process with `android context was not initialized`.

use crate::{Event, Settings, bind_client, client};
use jni::{
    JNIEnv,
    objects::{JClass, JObject, JString},
    sys::{jboolean, jint, jlong, JNI_FALSE, JNI_TRUE},
};
use std::{
    ffi::c_void,
    path::PathBuf,
    sync::{
        Arc, Mutex, OnceLock,
    },
    thread,
};
use tokio::sync::{mpsc, watch};

/// Tracks whether the JNI context handed to `ndk_context` is usable.
static ANDROID_CONTEXT: OnceLock<bool> = OnceLock::new();
/// Keeps the application `Context` alive: `ndk_context` stores a raw pointer to it.
static CONTEXT_REF: OnceLock<jni::objects::GlobalRef> = OnceLock::new();
/// Serialises installation; `ndk_context::initialize_android_context` asserts it runs only once.
static CONTEXT_LOCK: Mutex<()> = Mutex::new(());

/// What the Kotlin side implements: `fun onEvent(line: String)`.
///
/// Each line is one NDJSON object, exactly what the desktop helper prints on stdout:
/// `{"type":"ready","url":"https://127.0.0.1:1234"}` / `{"type":"path","path":"direct"}` /
/// `{"type":"error","message":"..."}`.
const SINK_METHOD: &str = "onEvent";
const SINK_SIGNATURE: &str = "(Ljava/lang/String;)V";

struct Tunnel {
    shutdown: watch::Sender<bool>,
}

/// Every live tunnel, keyed by the handle value handed to Kotlin.
static TUNNELS: Mutex<Vec<(i64, Arc<Tunnel>)>> = Mutex::new(Vec::new());
static NEXT_HANDLE: Mutex<i64> = Mutex::new(1);

fn next_handle() -> i64 {
    let mut next = NEXT_HANDLE.lock().unwrap_or_else(|e| e.into_inner());
    let value = *next;
    *next += 1;
    value
}

fn java_string(env: &mut JNIEnv, value: Option<JString>) -> Option<String> {
    let value = value?;
    env.get_string(&value).ok().map(Into::into)
}

/// Raw `JavaVM` pointer captured from the first `nativeInstallContext` call.
///
/// Stored as `usize` because `*mut sys::JavaVM` is neither `Send` nor `Sync`, while the pointer
/// itself is valid for the process lifetime (JNI guarantees the VM outlives any native call).
static JAVA_VM: OnceLock<usize> = OnceLock::new();

/// Hands the JVM and application `Context` to iroh's DNS resolver.
///
/// iroh reads the active network's DNS servers through JNI on Android. `ndk_context` has to be
/// initialised before the first endpoint is built, otherwise the resolver panics — and because
/// the release profile is `panic = "abort"` the process dies instead of falling back to the
/// default nameservers. Kotlin calls this once after loading the library and before
/// `nativeStart`.
///
/// Returns true when the context is available afterwards.
#[unsafe(no_mangle)]
pub extern "system" fn Java_local_wechatlite_mobile_net_TunnelProcess_nativeInstallContext<'local>(
    env: JNIEnv<'local>,
    _class: JClass<'local>,
    context: JObject<'local>,
) -> jboolean {
    let Ok(vm) = env.get_java_vm() else {
        return JNI_FALSE;
    };
    let Ok(global) = env.new_global_ref(&context) else {
        return JNI_FALSE;
    };
    // A second call (or a second tunnel) keeps the first reference; the duplicate is dropped.
    let _ = CONTEXT_REF.set(global);
    let _ = JAVA_VM.set(vm.get_java_vm_pointer() as usize);
    if ensure_android_context() { JNI_TRUE } else { JNI_FALSE }
}

/// Publishes the captured VM/context to `ndk_context` exactly once.
fn ensure_android_context() -> bool {
    if let Some(installed) = ANDROID_CONTEXT.get() {
        return *installed;
    }
    let _guard = CONTEXT_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(installed) = ANDROID_CONTEXT.get() {
        return *installed;
    }
    let installed = match (JAVA_VM.get(), CONTEXT_REF.get()) {
        (Some(vm), Some(context)) => {
            // SAFETY: the VM pointer comes from `GetJavaVM` and stays valid until the process
            // exits; `CONTEXT_REF` keeps the application context reachable for just as long, so
            // the raw pointers stored by `ndk_context` remain dereferenceable.
            unsafe {
                iroh::dns::install_android_jni_context(
                    *vm as *mut c_void,
                    context.as_obj().as_raw() as *mut c_void,
                );
            }
            true
        }
        _ => false,
    };
    let _ = ANDROID_CONTEXT.set(installed);
    installed
}

/// Starts a tunnel. Returns a non-zero handle on success, 0 on failure.
///
/// Failures are also reported through the sink so the UI can show the server's wording.
#[unsafe(no_mangle)]
pub extern "system" fn Java_local_wechatlite_mobile_net_TunnelProcess_nativeStart<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    server_id: JString<'local>,
    relay_url: JString<'local>,
    relay_token: JString<'local>,
    relay_ca: JString<'local>,
    relay_quic_port: jint,
    key_file: JString<'local>,
    sink: jni::objects::JObject<'local>,
) -> jlong {
    let report = |env: &mut JNIEnv, sink: &jni::objects::JObject, message: &str| {
        send_event(env, sink, &error_event(message));
    };

    // iroh resolves the relay host through the system DNS configuration, which needs the JNI
    // context. Without it the resolver panics and `panic = "abort"` kills the whole app.
    if !ensure_android_context() {
        report(&mut env, &sink, "自动连接组件缺少 Android 上下文");
        return 0;
    }

    let (Some(server_id), Some(relay_url), Some(relay_token), Some(relay_ca), Some(key_file)) = (
        java_string(&mut env, Some(server_id)),
        java_string(&mut env, Some(relay_url)),
        java_string(&mut env, Some(relay_token)),
        java_string(&mut env, Some(relay_ca)),
        java_string(&mut env, Some(key_file)),
    ) else {
        report(&mut env, &sink, "自动连接参数无效");
        return 0;
    };

    let settings = Settings {
        enabled: true,
        relay_url,
        relay_token,
        relay_ca,
        relay_quic_port: relay_quic_port.clamp(1, u16::MAX as jint) as u16,
    };
    if let Err(error) = settings.validate() {
        report(&mut env, &sink, &format!("入口配置无效：{error}"));
        return 0;
    }
    let Ok(endpoint_id) = server_id.parse() else {
        report(&mut env, &sink, "服务器设备标识无效");
        return 0;
    };
    let Ok(relay) = settings.relay_url.parse() else {
        report(&mut env, &sink, "中转地址无效");
        return 0;
    };
    let key_path = PathBuf::from(key_file);
    if !key_path.is_absolute() {
        report(&mut env, &sink, "密钥路径必须是绝对路径");
        return 0;
    }

    // The callback object must outlive this call, so hold a global reference for the tunnel's life.
    let Ok(sink_global) = env.new_global_ref(&sink) else {
        report(&mut env, &sink, "无法保持回调引用");
        return 0;
    };
    let Ok(java_vm) = env.get_java_vm() else {
        report(&mut env, &sink, "无法获取 JavaVM");
        return 0;
    };
    // jni 0.21's JavaVM is not Clone; share one handle across the worker and the forwarding task.
    let java_vm = Arc::new(java_vm);

    let handle = next_handle();
    let (shutdown, shutdown_rx) = watch::channel(false);
    let tunnel = Arc::new(Tunnel { shutdown });
    {
        let mut tunnels = TUNNELS.lock().unwrap_or_else(|e| e.into_inner());
        tunnels.push((handle, tunnel));
    }

    thread::Builder::new()
        .name("wechat-p2p".into())
        .spawn(move || {
            let runtime = match tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
            {
                Ok(runtime) => runtime,
                Err(error) => {
                    emit_java(&java_vm, &sink_global, &error_event(&format!("无法启动运行时：{error}")));
                    return;
                }
            };
            runtime.block_on(async move {
                let (events, mut receiver) = mpsc::channel::<Event>(32);
                let vm = Arc::clone(&java_vm);
                let callback = sink_global.clone();
                let forward = tokio::spawn(async move {
                    while let Some(event) = receiver.recv().await {
                        emit_java(&vm, &callback, &event_line(&event));
                    }
                });

                let result = match bind_client(&settings, &key_path, false).await {
                    Ok(endpoint) => client(endpoint, endpoint_id, relay, shutdown_rx, events.clone()).await,
                    Err(error) => Err(error),
                };
                if let Err(error) = result {
                    emit_java(&java_vm, &sink_global, &error_event(&error.to_string()));
                }
                drop(events);
                let _ = forward.await;
            });
        })
        .map_err(|error| report(&mut env, &sink, &format!("无法启动线程：{error}")))
        .ok();

    handle
}

/// Stops and drops a tunnel previously returned by `nativeStart`.
#[unsafe(no_mangle)]
pub extern "system" fn Java_local_wechatlite_mobile_net_TunnelProcess_nativeStop(
    _env: JNIEnv,
    _class: JClass,
    handle: jlong,
) {
    let mut tunnels = TUNNELS.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(index) = tunnels.iter().position(|(id, _)| *id == handle) {
        let (_, tunnel) = tunnels.remove(index);
        // Dropping the last sender makes the tunnel's `stopped()` future resolve and the runtime
        // task exit; the endpoint is closed by the client loop's shutdown path.
        tunnel.shutdown.send_replace(true);
    }
}

/// Reports whether the bundled library can actually start a tunnel on this device.
///
/// The library is only packaged for arm64, so reaching this function at all already means the
/// payload matches the device ABI; a missing symbol would fail at `System.loadLibrary` instead.
#[unsafe(no_mangle)]
pub extern "system" fn Java_local_wechatlite_mobile_net_TunnelProcess_nativeAvailable(
    _env: JNIEnv,
    _class: JClass,
) -> jboolean {
    JNI_TRUE
}

fn event_line(event: &Event) -> String {
    match event {
        Event::Ready { url } => format!("{{\"type\":\"ready\",\"url\":{}}}", json_string(url)),
        Event::Path { path } => format!("{{\"type\":\"path\",\"path\":{}}}", json_string(path)),
        // `Event::Error` carries a &'static str from the tunnel core.
        Event::Error { message } => error_event(message),
    }
}

fn error_event(message: &str) -> String {
    // Keep the desktop helper's user-facing wording for endpoint/relay failures, which never carry
    // actionable detail and may include configuration hints.
    let text = if message.contains("P2P") || message.contains("relay") {
        "P2P link failed; check endpoint identity, relay authorization and TLS certificate"
    } else {
        message
    };
    format!("{{\"type\":\"error\",\"message\":{}}}", json_string(text))
}

/// Calls `sink.onEvent(line)` from the tunnel thread.
///
/// The thread is attached **permanently** rather than per call: `jni` 0.21's per-call guard
/// detaches on drop, and tokio reuses its worker threads for many tasks, so a detach between two
/// events can leave a later call on a thread the VM no longer knows — which aborts the process
/// with `android context was not initialized` instead of returning an error.
fn emit_java(vm: &jni::JavaVM, sink: &jni::objects::GlobalRef, line: &str) {
    let Ok(mut env) = vm.attach_current_thread_permanently() else { return };
    send_event(&mut env, sink.as_obj(), line);
}

fn send_event(env: &mut JNIEnv, sink: &jni::objects::JObject, line: &str) {
    let Ok(text) = env.new_string(line) else { return };
    let result = env.call_method(sink, SINK_METHOD, SINK_SIGNATURE, &[(&text).into()]);
    // A throwing receiver must not poison the tunnel thread. `call_method` reports the throw as
    // `Err` and leaves the exception pending, so the clear has to happen on both paths: a pending
    // exception left behind aborts the process on the next JNI call from this thread.
    if env.exception_check().unwrap_or(false) {
        let _ = env.exception_clear();
    }
    let _ = result;
}

/// Minimal JSON string escaping; payloads only contain identifiers, URLs and messages.
fn json_string(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if (ch as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", ch as u32)),
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}
