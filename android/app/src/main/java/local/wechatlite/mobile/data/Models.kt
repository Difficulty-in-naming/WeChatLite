package local.wechatlite.mobile.data

import org.json.JSONArray
import org.json.JSONObject

/** One transferable attachment, mirroring `Media` in server/src/model.rs. */
data class Media(
    val id: String,
    val name: String,
    val kind: String,
    val size: Long,
    val sha256: String,
    val localPath: String = "",
    /** Local-only path of the small poster the server generates for a video. */
    val posterPath: String = "",
) {
    val isImage: Boolean get() = kind == "image"
    val isGif: Boolean get() = kind == "gif"
    val isVideo: Boolean get() = kind == "video"
    val isVoice: Boolean get() = kind == "voice"

    companion object {
        fun from(json: JSONObject) = Media(
            id = json.optString("id"),
            name = json.optString("name"),
            kind = json.optString("kind", "file"),
            size = json.optLong("size"),
            sha256 = json.optString("sha256"),
            // Present only in rows restored from [MessageCache]; the relay never sends them.
            localPath = json.optString("local_path"),
            posterPath = json.optString("poster_path"),
        )
    }

    /**
     * Serialised form used by [MessageCache]. The two local paths are included so a cached row can
     * still draw its thumbnail straight after a restart, instead of waiting for a new download.
     */
    fun toJson(): JSONObject = JSONObject()
        .put("id", id)
        .put("name", name)
        .put("kind", kind)
        .put("size", size)
        .put("sha256", sha256)
        .apply {
            if (localPath.isNotEmpty()) put("local_path", localPath)
            if (posterPath.isNotEmpty()) put("poster_path", posterPath)
        }
}

/** A member of the selected group, used for real @mentions. */
data class Member(
    val id: String,
    val name: String,
    val remark: String = "",
    val avatar: String = "",
) {
    companion object {
        fun from(json: JSONObject) = Member(
            id = json.optString("id"),
            name = json.optString("name"),
            remark = json.optString("remark"),
            avatar = json.optString("avatar"),
        )
    }
}

/** A conversation the current client key may read. */
data class Group(
    val id: String,
    val name: String,
    /** Member count reported by WeChat, not an unread count. */
    val count: Int = 0,
    val avatar: String = "",
    /** Sender-prefixed text of the newest message, as summarised by the server. */
    val preview: String = "",
    /** Timestamp of the newest message, used for the row's clock and the desktop ordering. */
    val timestamp: Long = 0,
    /** Local-only unread counter, following the desktop sidebar's red badge. */
    val unread: Int = 0,
    /** Local-only subset of [unread] that mentioned this account. */
    val unreadMentions: Int = 0,
    /** Local-only pin state, persisted in settings and drawn like the desktop sidebar. */
    val pinned: Boolean = false,
) {
    companion object {
        fun from(json: JSONObject) = Group(
            id = json.optString("id"),
            name = json.optString("name"),
            count = json.optInt("count"),
            avatar = json.optString("avatar"),
            preview = json.optString("preview"),
            timestamp = json.optLong("timestamp"),
        )
    }
}

/** A quoted source. `unavailable` becomes true once the original is recalled. */
data class Quote(
    val id: String,
    val sender: String = "",
    val text: String = "",
    val unavailable: Boolean = false,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("id", id)
        .put("sender", sender)
        .put("text", text)
        .put("unavailable", unavailable)

    companion object {
        fun from(json: JSONObject) = Quote(
            id = json.optString("id"),
            sender = json.optString("sender"),
            text = json.optString("text"),
            unavailable = json.optBoolean("unavailable"),
        )
    }
}

/** One child entry of a merged forward card. */
data class ForwardItem(
    val id: String,
    val sender: String = "",
    val text: String = "",
    val timestamp: Long = 0,
    val media: List<Int> = emptyList(),
) {
    fun toJson(): JSONObject = JSONObject()
        .put("id", id)
        .put("sender", sender)
        .put("text", text)
        .put("timestamp", timestamp)
        .put("media", JSONArray().apply { media.forEach { put(it) } })

    companion object {
        fun from(json: JSONObject) = ForwardItem(
            id = json.optString("id"),
            sender = json.optString("sender"),
            text = json.optString("text"),
            timestamp = json.optLong("timestamp"),
            media = json.optJSONArray("media")?.let { array ->
                (0 until array.length()).map { array.optInt(it, -1) }.filter { it >= 0 }
            } ?: emptyList(),
        )
    }
}

/** A read-only merged chat record card. */
data class ForwardRecord(
    val title: String,
    val items: List<ForwardItem> = emptyList(),
    val pending: Boolean = false,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("title", title)
        .put("pending", pending)
        .put("items", JSONArray().apply { items.forEach { put(it.toJson()) } })

    companion object {
        fun from(json: JSONObject) = ForwardRecord(
            title = json.optString("title"),
            items = json.optJSONArray("items")?.let { array ->
                (0 until array.length()).mapNotNull { index ->
                    array.optJSONObject(index)?.let(ForwardItem::from)
                }
            } ?: emptyList(),
            pending = json.optBoolean("pending"),
        )
    }
}

/** A stored chat message, mirroring `ChatMessage` in server/src/model.rs. */
data class ChatMessage(
    val id: String,
    val group: String,
    val sender: String,
    val senderId: String = "",
    val avatar: String = "",
    val text: String = "",
    val media: List<Media> = emptyList(),
    val mentions: List<Member> = emptyList(),
    val quote: Quote? = null,
    val record: ForwardRecord? = null,
    val timestamp: Long = 0,
    val status: String = "received",
    val error: String = "",
    val sourceId: String = "",
    val recallStatus: String = "",
    val recallError: String = "",
    val recallRetryable: Boolean = false,
    val outgoing: Boolean = false,
    val seq: Long = 0,
) {
    val isRecalled: Boolean get() = status == "recalled"
    val isPending: Boolean get() = status == "queued" || status == "sending"

    /** True when this client may ask the server to recall the message right now. */
    fun canRecall(now: Long = System.currentTimeMillis()): Boolean {
        if (!outgoing || isRecalled) return false
        if (status != "sent" && status != "received") return false
        if (text.isEmpty() || media.isNotEmpty() || quote != null || record != null) return false
        if (sourceId.isEmpty() || id.isEmpty()) return false
        if (recallStatus == "pending") return false
        if (recallStatus == "uncertain") return false
        if (recallStatus == "failed" && !recallRetryable) return false
        return now - timestamp <= RECALL_WINDOW_MS
    }

    companion object {
        const val RECALL_WINDOW_MS = 180_000L

        /**
         * Serialised form for [MessageCache]. Field names match what the relay sends, so
         * [from] reads a cached row exactly like a server row.
         */
        fun toJson(message: ChatMessage): JSONObject = JSONObject()
            .put("id", message.id)
            .put("group", message.group)
            .put("sender", message.sender)
            .put("sender_id", message.senderId)
            .put("avatar", message.avatar)
            .put("text", message.text)
            .put("timestamp", message.timestamp)
            .put("status", message.status)
            .put("outgoing", message.outgoing)
            .put("seq", message.seq)
            .apply {
                if (message.error.isNotEmpty()) put("error", message.error)
                if (message.sourceId.isNotEmpty()) put("source_id", message.sourceId)
                if (message.recallStatus.isNotEmpty()) put("recall_status", message.recallStatus)
                if (message.recallError.isNotEmpty()) put("recall_error", message.recallError)
                if (message.recallRetryable) put("recall_retryable", true)
                if (message.media.isNotEmpty()) {
                    put("media", JSONArray().apply { message.media.forEach { put(it.toJson()) } })
                }
                if (message.mentions.isNotEmpty()) {
                    put("mentions", JSONArray().apply {
                        message.mentions.forEach { member ->
                            put(JSONObject().put("id", member.id).put("name", member.name))
                        }
                    })
                }
                message.quote?.let { put("quote", it.toJson()) }
                message.record?.let { put("record", it.toJson()) }
            }
        fun from(json: JSONObject) = ChatMessage(
            id = json.optString("id"),
            group = json.optString("group"),
            sender = json.optString("sender"),
            senderId = json.optString("sender_id"),
            avatar = json.optString("avatar"),
            text = json.optString("text"),
            media = json.optJSONArray("media").toObjectList(Media::from),
            mentions = json.optJSONArray("mentions").toObjectList(Member::from),
            quote = json.optJSONObject("quote")?.let(Quote::from),
            record = json.optJSONObject("record")?.let(ForwardRecord::from),
            timestamp = json.optLong("timestamp"),
            status = json.optString("status", "received"),
            error = json.optString("error"),
            sourceId = json.optString("source_id"),
            recallStatus = json.optString("recall_status"),
            recallError = json.optString("recall_error"),
            recallRetryable = json.optBoolean("recall_retryable"),
            outgoing = json.optBoolean("outgoing"),
            seq = json.optLong("seq"),
        )
    }
}

/** Server capability and permission snapshot returned by `GET /api/status`. */
data class ServerStatus(
    val ready: Boolean = false,
    val text: String = "",
    val selfId: String = "",
    val recallSupported: Boolean = true,
    val recallReason: String = "",
    val clientName: String = "",
    val isAdmin: Boolean = false,
    val allowedGroups: Set<String> = emptySet(),
    val canSend: Boolean = false,
    val canDownload: Boolean = false,
    val onlineClients: Int = 0,
) {
    companion object {
        fun from(json: JSONObject): ServerStatus {
            val permissions = json.optJSONObject("permissions")
            val client = json.optJSONObject("authenticated_client")
            return ServerStatus(
                ready = json.optBoolean("ready"),
                text = json.optString("text"),
                selfId = json.optString("self_id"),
                recallSupported = json.optBoolean("recall_supported", true),
                recallReason = json.optString("recall_reason"),
                clientName = client?.optString("name") ?: "",
                isAdmin = client?.optBoolean("admin") ?: false,
                allowedGroups = permissions?.optJSONArray("groups")?.let { array ->
                    (0 until array.length()).mapNotNull { array.optString(it).takeIf(String::isNotEmpty) }.toSet()
                } ?: emptySet(),
                canSend = permissions?.optBoolean("can_send") ?: false,
                canDownload = permissions?.optBoolean("can_download") ?: false,
                onlineClients = json.optInt("online_clients"),
            )
        }
    }
}

/** A page of older messages from `GET /api/history`. */
data class HistoryPage(
    val messages: List<ChatMessage>,
    val hasMore: Boolean,
    val pending: Boolean,
) {
    companion object {
        fun from(json: JSONObject): HistoryPage {
            // Older servers returned a bare array; keep reading it rather than failing the page.
            val array = json.optJSONArray("messages")
            return HistoryPage(
                messages = array.toObjectList(ChatMessage::from),
                hasMore = json.optBoolean("has_more") || json.optBoolean("pending"),
                pending = json.optBoolean("pending"),
            )
        }
    }
}

/** A page from the server-side chat record search. */
data class RecordPage(
    val messages: List<ChatMessage>,
    val nextBefore: String?,
    val hasMore: Boolean,
    val pending: Boolean,
) {
    companion object {
        fun from(json: JSONObject): RecordPage {
            val next = json.optString("next_before").takeIf { it.isNotEmpty() }
            return RecordPage(
                messages = json.optJSONArray("messages").toObjectList(ChatMessage::from),
                nextBefore = next,
                hasMore = json.optBoolean("has_more") || json.optBoolean("pending"),
                pending = json.optBoolean("pending"),
            )
        }
    }
}

/** The group announcement, which distinguishes "not set" from "not readable". */
data class Announcement(
    val group: String,
    val available: Boolean,
    val set: Boolean,
    val author: String,
    val authorId: String,
    val avatar: String = "",
    val updatedAt: Long? = null,
    val text: String = "",
    val pending: Boolean = false,
) {
    companion object {
        fun from(json: JSONObject) = Announcement(
            group = json.optString("group"),
            available = json.optBoolean("available"),
            set = json.optBoolean("set"),
            author = json.optString("author"),
            authorId = json.optString("author_id"),
            avatar = json.optString("avatar"),
            updatedAt = json.optLong("updated_at").takeIf { it > 0 },
            text = json.optString("text"),
            pending = json.optBoolean("pending"),
        )
    }
}

/** A server event delivered on `/api/events`. */
sealed interface ServerEvent {
    val seq: Long

    data class Message(val message: ChatMessage, val historical: Boolean, override val seq: Long) : ServerEvent
    data class Groups(override val seq: Long) : ServerEvent
    data class Members(val group: String, override val seq: Long) : ServerEvent
    data class Status(val status: ServerStatus, override val seq: Long) : ServerEvent
    data class Warning(val text: String, val group: String, override val seq: Long) : ServerEvent
    data class Reset(override val seq: Long) : ServerEvent
    data class Synced(override val seq: Long) : ServerEvent
    data class Unknown(val type: String, override val seq: Long) : ServerEvent

    companion object {
        fun from(json: JSONObject): ServerEvent {
            val seq = json.optLong("seq")
            return when (json.optString("type")) {
                "message" -> json.optJSONObject("message")?.let {
                    Message(ChatMessage.from(it), json.optBoolean("historical"), seq)
                } ?: Unknown("message", seq)

                "groups" -> Groups(seq)
                "members" -> Members(json.optString("group"), seq)
                "status" -> Status(ServerStatus.from(json), seq)
                "warning" -> Warning(json.optString("text"), json.optString("group"), seq)
                "reset" -> Reset(seq)
                "synced" -> Synced(seq)
                else -> Unknown(json.optString("type"), seq)
            }
        }
    }
}

internal inline fun <T> JSONArray?.toObjectList(transform: (JSONObject) -> T): List<T> {
    if (this == null) return emptyList()
    val items = ArrayList<T>(length())
    for (index in 0 until length()) {
        optJSONObject(index)?.let { items.add(transform(it)) }
    }
    return items
}
