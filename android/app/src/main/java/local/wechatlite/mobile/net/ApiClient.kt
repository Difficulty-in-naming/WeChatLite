package local.wechatlite.mobile.net

import java.io.IOException
import java.io.InputStream
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import local.wechatlite.mobile.data.ConnectionConfig
import local.wechatlite.mobile.data.ServerEvent
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONArray
import org.json.JSONObject
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** A server response body plus its HTTP status. */
class ApiException(message: String, val status: Int = 0) : Exception(message)

/**
 * One authenticated connection to the relay.
 *
 * Every request carries the client's own bearer key; the administrator secret is deliberately not
 * accepted here because it is loopback-only on the server and must never be copied to a phone.
 */
class ApiClient(config: ConnectionConfig, baseUrl: String) {
    private val token = config.token
    private val certificatePin: String? = config.certificatePin
    private val client: OkHttpClient = PinnedTls.client(certificatePin)
    private val eventClient: OkHttpClient = client.newBuilder()
        // A WebSocket is idle between heartbeats; the server pings every 30 seconds.
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .build()
    private val base: HttpUrl = baseUrl.toHttpUrl()
    private val jsonType = "application/json; charset=utf-8".toMediaType()

    private fun url(path: String, query: Map<String, String> = emptyMap()): HttpUrl {
        val builder = base.newBuilder()
        // `path` is always a server-relative path such as "/api/status".
        builder.encodedPath(base.encodedPath.trimEnd('/') + path)
        query.forEach { (key, value) -> builder.addQueryParameter(key, value) }
        return builder.build()
    }

    private fun request(path: String, query: Map<String, String> = emptyMap()): Request.Builder =
        Request.Builder()
            .url(url(path, query))
            .header("Authorization", "Bearer $token")

    suspend fun get(path: String, query: Map<String, String> = emptyMap()): JSONObject =
        execute(request(path, query).get().build())

    /**
     * Some endpoints (`/api/groups`, `/api/groups/{id}/members`) answer with a bare JSON array
     * instead of an object, so they need their own parse step.
     */
    suspend fun getArray(path: String, query: Map<String, String> = emptyMap()): JSONArray =
        executeRaw(request(path, query).get().build()) { response ->
            val text = response.body?.string().orEmpty()
            try {
                JSONArray(text)
            } catch (e: Exception) {
                throw ApiException("服务器响应不是有效 JSON 数组", response.code)
            }
        }

    suspend fun postJson(path: String, body: JSONObject, query: Map<String, String> = emptyMap()): JSONObject =
        execute(request(path, query).post(body.toString().toRequestBody(jsonType)).build())

    /** Streams `GET /api/media/{id}` straight to [sink], hashing and size-checking is the caller's job. */
    suspend fun download(path: String, query: Map<String, String> = emptyMap(), sink: (InputStream) -> Unit) {
        withContext(Dispatchers.IO) {
            executeRaw(request(path, query).get().build()) { response ->
                val body = response.body ?: throw ApiException("附件响应为空", response.code)
                body.byteStream().use(sink)
                Unit
            }
        }
    }

    /** Uploads one attachment with a declared Content-Length, matching the server's expectation. */
    suspend fun upload(path: String, query: Map<String, String>, body: RequestBody): JSONObject =
        execute(request(path, query).post(body).build())

    private suspend fun execute(request: Request): JSONObject = executeRaw(request) { response ->
        val text = response.body?.string().orEmpty()
        if (text.isEmpty()) JSONObject() else runCatching { JSONObject(text) }
            .getOrElse { throw ApiException("服务器响应不是有效 JSON", response.code) }
    }

    private suspend fun <T> executeRaw(request: Request, read: (Response) -> T): T =
        withContext(Dispatchers.IO) {
            suspendCancellableCoroutine { continuation ->
                val call = client.newCall(request)
                continuation.invokeOnCancellation { call.cancel() }
                call.enqueue(object : Callback {
                    override fun onFailure(call: Call, e: IOException) {
                        if (continuation.isActive) {
                            continuation.resumeWithException(describe(e))
                        }
                    }
                    override fun onResponse(call: Call, response: Response) {
                        response.use { it ->
                            if (!it.isSuccessful) {
                                val message = runCatching {
                                    JSONObject(it.body?.string().orEmpty()).optString("error")
                                }.getOrDefault("")
                                if (continuation.isActive) {
                                    continuation.resumeWithException(
                                        ApiException(message.ifEmpty { "服务器返回 ${it.code}" }, it.code),
                                    )
                                }
                                return
                            }
                            if (!continuation.isActive) return
                            try {
                                continuation.resume(read(it))
                            } catch (e: Exception) {
                                continuation.resumeWithException(e)
                            }
                        }
                    }
                })
            }
        }

    /**
     * OkHttp wraps a failing trust manager inside an SSLHandshakeException, so walk the cause chain
     * instead of testing the outer type: a pin mismatch is a specific, actionable failure.
     */
    private fun describe(e: IOException): ApiException {
        var cause: Throwable? = e
        while (cause != null) {
            if (cause is CertificatePinException) {
                return ApiException("服务器证书指纹不匹配，已拒绝连接")
            }
            cause = cause.cause
        }
        val text = e.message.orEmpty()
        return when {
            text.contains("Trust anchor", ignoreCase = true) ||
                text.contains("certificate", ignoreCase = true) ||
                text.contains("SSL", ignoreCase = true) ->
                ApiException("服务器证书校验失败，请检查服务端证书或配置证书 SHA-256 指纹")

            e is java.net.SocketTimeoutException -> ApiException("连接服务器超时，请检查地址与网络")
            e is java.net.UnknownHostException -> ApiException("无法解析服务器地址")
            e is java.net.ConnectException -> ApiException("无法连接服务器，请确认服务已启动且端口可达")
            else -> ApiException(text.ifEmpty { "网络连接失败" })
        }
    }

    /**
     * Opens the durable event stream. The caller owns reconnection; [after] is the last seen
     * sequence so a reconnect resumes rather than replaying everything.
     */
    fun events(after: Long): Flow<ServerEvent> = callbackFlow {
        val httpUrl = url("/api/events", mapOf("after" to after.toString()))
        val request = Request.Builder()
            .url(httpUrl)
            .header("Authorization", "Bearer $token")
            .build()

        val socket = eventClient.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) {
                val event = runCatching { ServerEvent.from(JSONObject(text)) }.getOrNull() ?: return
                trySend(event)
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                close(describe(t as? IOException ?: IOException(t.message)))
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                close(ApiException("事件连接已关闭：$reason"))
            }
        })

        awaitClose { socket.close(1000, "client closed") }
    }

    /**
     * Releases sockets and executors.
     *
     * `evictAll` closes live TLS connections synchronously, which performs a socket write; calling
     * it on the main thread throws `NetworkOnMainThreadException` and kills the process during
     * `onCleared`. It therefore always runs on an IO thread, and every step is guarded so a closing
     * socket can never turn teardown into a crash.
     */
    fun close() {
        CoroutineScope(Dispatchers.IO).launch {
            runCatching { eventClient.dispatcher.executorService.shutdown() }
            runCatching { eventClient.connectionPool.evictAll() }
            runCatching { client.dispatcher.executorService.shutdown() }
            runCatching { client.connectionPool.evictAll() }
        }
    }
}
