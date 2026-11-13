package local.wechatlite.mobile.net

import android.content.Context
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import local.wechatlite.mobile.data.ConnectionConfig
import org.json.JSONObject

/** Raised when the tunnel cannot start or reports a failure. */
class TunnelException(message: String) : Exception(message)

/**
 * Runs the P2P tunnel **inside the app process** through JNI.
 *
 * Android cannot execute a bundled ELF binary from app storage: `untrusted_app` may not run files
 * under `app_data_file`, and packaging it under `lib/` makes the loader treat it as a shared
 * library, aborting with `android context was not initialized`. The supported shape is a cdylib
 * loaded by `System.loadLibrary`; the tunnel then runs on its own tokio runtime in-process.
 *
 * The Rust side reports events through a C callback carrying one NDJSON line, which mirrors the
 * desktop helper's stdout protocol.
 */
class TunnelProcess(private val context: Context) {
    private var handle: Long = 0L

    /** Starts the tunnel and returns the local `https://127.0.0.1:<port>` endpoint. */
    suspend fun start(config: ConnectionConfig.P2p): String = withContext(Dispatchers.IO) {
        stop()
        ensureLoaded()

        val keyFile = File(context.filesDir, "p2p-client.key").apply { parentFile?.mkdirs() }
        val ready = AtomicReference<String?>(null)
        val failure = AtomicReference<String?>(null)
        val latch = CountDownLatch(1)

        val handleValue = nativeStart(
            serverId = config.serverId,
            relayUrl = config.relayUrl,
            relayToken = config.relayToken,
            relayCa = config.relayCa,
            relayQuicPort = config.relayQuicPort,
            keyFile = keyFile.absolutePath,
        ) { line ->
            val event = runCatching { JSONObject(line) }.getOrNull() ?: return@nativeStart
            when (event.optString("type")) {
                "ready" -> {
                    ready.set(event.optString("url"))
                    latch.countDown()
                }

                "error" -> {
                    failure.set(redact(event.optString("message"), config))
                    latch.countDown()
                }

                // "path" is informational only, matching the desktop helper.
                else -> Unit
            }
        }

        if (handleValue == 0L) {
            throw TunnelException("无法启动自动连接组件")
        }
        handle = handleValue

        if (!latch.await(STARTUP_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
            stop()
            throw TunnelException("自动连接启动超时，请检查公网入口和网络")
        }
        failure.get()?.let { message ->
            stop()
            throw TunnelException(message)
        }
        val url = ready.get()
        if (url == null || !url.startsWith("https://127.0.0.1:")) {
            stop()
            throw TunnelException("自动连接组件返回了无效的本机地址")
        }
        url
    }

    /** True when this build actually ships the tunnel library for the device ABI. */
    fun isAvailable(): Boolean = runCatching {
        ensureLoaded()
        nativeAvailable()
    }.getOrDefault(false)

    fun stop() {
        val current = handle
        if (current == 0L) return
        handle = 0L
        runCatching { nativeStop(current) }
    }

    private fun ensureLoaded() {
        if (loaded) return
        synchronized(lock) {
            if (loaded) return
            System.loadLibrary("wechat_p2p")
            // iroh reads the system DNS servers over JNI and needs this context before it builds
            // an endpoint. The release build uses `panic = "abort"`, so a missing context would
            // abort the process instead of falling back to default nameservers.
            if (!nativeInstallContext(context.applicationContext)) {
                throw TunnelException("自动连接组件无法获取 Android 上下文")
            }
            loaded = true
        }
    }

    private fun redact(message: String, config: ConnectionConfig.P2p): String {
        val safe = message.replace(config.relayToken, "[密钥已隐藏]")
        return safe.filter { it.code >= 0x20 && it.code != 0x7f }.take(500)
    }

    /** Receiver for one NDJSON event line from Rust; returns immediately. */
    private fun interface EventSink {
        fun onEvent(line: String)
    }

    private external fun nativeStart(
        serverId: String,
        relayUrl: String,
        relayToken: String,
        relayCa: String,
        relayQuicPort: Int,
        keyFile: String,
        sink: EventSink,
    ): Long

    private external fun nativeInstallContext(context: android.content.Context): Boolean

    private external fun nativeStop(handle: Long)

    private external fun nativeAvailable(): Boolean

    private companion object {
        const val STARTUP_TIMEOUT_MS = 60_000L
        val lock = Any()
        @Volatile
        var loaded = false
    }
}
