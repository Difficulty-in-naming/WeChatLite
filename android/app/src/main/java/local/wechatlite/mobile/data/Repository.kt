package local.wechatlite.mobile.data

import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.security.MessageDigest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import local.wechatlite.mobile.net.ApiClient
import local.wechatlite.mobile.net.ApiException
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.asRequestBody
import org.json.JSONArray
import org.json.JSONObject

/**
 * Typed access to the relay's REST surface.
 *
 * Every call keeps the desktop client's semantics: the same endpoints, the same query parameters,
 * and the same rule that reads require group access while writes additionally require `can_send`.
 */
class Repository(private val api: ApiClient, private val cacheDir: File) {

    /** Messages written by previous sessions, drawn before the first network page arrives. */
    val messages = MessageCache(cacheDir)

    suspend fun status(): ServerStatus = ServerStatus.from(api.get("/api/status"))

    suspend fun groups(): List<Group> =
        api.getArray("/api/groups").toObjectList(Group::from)

    suspend fun members(group: String): List<Member> =
        api.getArray("/api/groups/${encode(group)}/members").toObjectList(Member::from)

    suspend fun announcement(group: String): Announcement =
        Announcement.from(api.get("/api/groups/${encode(group)}/announcement"))

    /** Asks the server to make the group's native history readable before paging through it. */
    suspend fun activate(group: String) {
        api.postJson("/api/groups/${encode(group)}/activate", JSONObject())
    }

    /**
     * Reads up to 60 older messages. `before` is a message sequence; 0 starts from the newest page.
     * A `pending` page means the server is still pulling from WeChat and the same cursor may be
     * retried — the desktop client retries a bounded number of times for exactly that reason.
     */
    suspend fun history(group: String, before: Long = 0): HistoryPage {
        val json = api.get(
            "/api/history",
            mapOf("group" to group, "before" to before.toString()),
        )
        return HistoryPage.from(json)
    }

    /**
     * The newest page the relay already has on disk, without asking WeChat for anything.
     *
     * This is `GET /api/messages`, the same endpoint the desktop history browser pages through; it
     * never triggers a native pull, so it is safe to call for a listing whose rows carry no
     * summary yet. Returns the oldest-first page exactly like [history].
     */
    suspend fun cachedHistory(group: String): List<ChatMessage> =
        api.getArray("/api/messages", mapOf("group" to group, "before" to "0"))
            .toObjectList(ChatMessage::from)

    /** Server-side search across stored history, including filters the desktop record window uses. */
    suspend fun records(
        group: String,
        query: String = "",
        kind: String = "all",
        before: String = "",
        member: String = "",
        start: Long? = null,
        end: Long? = null,
        limit: Int = 50,
    ): RecordPage {
        val params = buildMap {
            put("group", group)
            put("limit", limit.toString())
            if (query.isNotEmpty()) put("q", query)
            if (kind != "all") put("type", kind)
            if (before.isNotEmpty()) put("before", before)
            if (member.isNotEmpty()) put("member", member)
            start?.let { put("start", it.toString()) }
            end?.let { put("end", it.toString()) }
        }
        return RecordPage.from(api.get("/api/records", params))
    }

    /** One stored message, used to jump from a search result or quote back to its source. */
    suspend fun message(group: String, id: String): ChatMessage =
        ChatMessage.from(api.get("/api/messages/${encode(id)}", mapOf("group" to group)))

    /** Starts a recall. The reply already carries the new `pending` recall state. */
    suspend fun recall(group: String, id: String): ChatMessage =
        ChatMessage.from(api.postJson("/api/messages/${encode(id)}/recall", JSONObject(), mapOf("group" to group)))

    /**
     * Uploads one attachment and returns the stored media descriptor. The server enforces the group
     * and `can_send` while streaming as well as before committing the file.
     */
    suspend fun upload(group: String, file: File, name: String = file.name): Media {
        val body: RequestBody = file.asRequestBody("application/octet-stream".toMediaType())
        val json = api.upload(
            "/api/media",
            mapOf("group" to group, "name" to name),
            body,
        )
        return Media.from(json)
    }

    /** Sends a message. The `id` must stay stable across retries so the server can deduplicate. */
    suspend fun send(
        id: String,
        group: String,
        sender: String,
        clientId: String,
        text: String,
        media: List<String> = emptyList(),
        mentions: List<Member> = emptyList(),
        quoteId: String? = null,
    ): ChatMessage {
        val body = JSONObject()
            .put("id", id)
            .put("group", group)
            .put("client_id", clientId)
            .put("sender", sender)
            .put("text", text)
            .put("media", JSONArray(media))
            .put("mentions", JSONArray().apply {
                mentions.forEach {
                    put(JSONObject().put("id", it.id).put("name", it.name))
                }
            })
        quoteId?.let { body.put("quote", JSONObject().put("id", it)) }
        return ChatMessage.from(api.postJson("/api/messages", body))
    }

    /**
     * Returns the attachment when it is already on disk, without touching the network. A file the
     * user opened before is preferred over the cache copy, and a cached file only counts when its
     * length still matches the descriptor.
     */
    fun cached(media: Media, group: String): File? {
        media.localPath.takeIf { it.isNotEmpty() }?.let(::File)?.takeIf { it.isFile }?.let { return it }
        val target = File(File(cacheDir, "media/${sanitize(group)}"), "${media.id}-${sanitize(media.name)}")
        return target.takeIf { it.isFile && (media.size <= 0 || it.length() == media.size) }
    }

    /**
     * Downloads an attachment into the app cache, verifying the advertised size and SHA-256 before
     * handing the file to any viewer. The desktop client performs the same verification.
     */
    suspend fun download(media: Media, group: String, onProgress: (Long) -> Unit = {}): File =
        withContext(Dispatchers.IO) {
            cached(media, group)?.let { return@withContext it }
            val directory = File(cacheDir, "media/${sanitize(group)}").apply { mkdirs() }
            val target = File(directory, "${media.id}-${sanitize(media.name)}")
            val partial = File(directory, "${target.name}.part")
            val digest = MessageDigest.getInstance("SHA-256")
            var written = 0L
            try {
                api.download("/api/media/${encode(media.id)}", mapOf("group" to group)) { input: InputStream ->
                    FileOutputStream(partial).use { output ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = input.read(buffer)
                            if (read < 0) break
                            output.write(buffer, 0, read)
                            digest.update(buffer, 0, read)
                            written += read
                            onProgress(written)
                        }
                        output.fd.sync()
                    }
                }
            } catch (e: Exception) {
                partial.delete()
                throw e
            }
            if (media.size > 0 && written != media.size) {
                partial.delete()
                throw ApiException("附件大小不一致，已丢弃")
            }
            val actual = digest.digest().joinToString("") { "%02x".format(it) }
            if (media.sha256.isNotEmpty() && !actual.equals(media.sha256, ignoreCase = true)) {
                partial.delete()
                throw ApiException("附件校验失败，已丢弃")
            }
            if (target.exists()) target.delete()
            if (!partial.renameTo(target)) {
                partial.delete()
                throw ApiException("无法保存附件")
            }
            target
        }

    /** Returns an already downloaded video poster without touching the network. */
    fun cachedPoster(media: Media, group: String): File? {
        media.posterPath.takeIf { it.isNotEmpty() }?.let(::File)?.takeIf { it.isFile }?.let { return it }
        val target = File(File(cacheDir, "posters/${sanitize(group)}"), "${media.id}.jpg")
        return target.takeIf { it.isFile && it.length() > 0 }
    }

    /**
     * Returns whatever a chat row can draw right now without a download: the full file for an image
     * or GIF thumbnail, or the small poster for a video card.
     */
    fun cachedPreview(media: Media, group: String): File? =
        if (media.isVideo) cachedPoster(media, group) else cached(media, group)

    /** Downloads the small JPEG poster the server generates for a video card. */
    suspend fun poster(media: Media, group: String): File = withContext(Dispatchers.IO) {
        cachedPoster(media, group)?.let { return@withContext it }
        val directory = File(cacheDir, "posters/${sanitize(group)}").apply { mkdirs() }
        val target = File(directory, "${media.id}.jpg")
        val partial = File(directory, "${media.id}.jpg.part")
        try {
            api.download("/api/media/${encode(media.id)}/poster", mapOf("group" to group)) { input ->
                FileOutputStream(partial).use { output -> input.copyTo(output) }
            }
        } catch (e: Exception) {
            partial.delete()
            throw e
        }
        if (partial.length() == 0L) {
            partial.delete()
            throw ApiException("封面不可用")
        }
        if (target.exists()) target.delete()
        if (!partial.renameTo(target)) {
            partial.delete()
            throw ApiException("无法保存封面")
        }
        target
    }

    private fun encode(value: String): String = java.net.URLEncoder.encode(value, "UTF-8").replace("+", "%20")

    private fun sanitize(value: String): String =
        value.map { if (it.isLetterOrDigit() || it == '-' || it == '_' || it == '.') it else '_' }
            .joinToString("")
            .take(80)
            .ifEmpty { "item" }
}
