package local.wechatlite.mobile.data

import java.io.File
import org.json.JSONArray

/**
 * Last known messages per conversation, kept on the device.
 *
 * The relay already stores messages, but reading them over the network is what made a conversation
 * open blank for a moment: the list starts empty on every open and the first page only arrives after
 * a round trip (and, when the relay's own cache is short, after a wait for WeChat). Drawing the
 * previous page from disk first makes the open instant, then the network result replaces it.
 *
 * This mirrors the desktop's behaviour of keeping its rows in memory during a session; the phone
 * additionally writes them down because the process is killed far more often.
 *
 * Files are per conversation and bounded, so a large number of groups cannot grow without limit.
 * Nothing here is authoritative: every write is best-effort and a corrupt file is simply ignored.
 */
class MessageCache(private val root: File) {

    private fun fileFor(group: String): File =
        File(File(root, "messages"), sanitize(group) + ".json")

    /** The messages written for this conversation, newest last. Empty when nothing usable is stored. */
    fun read(group: String): List<ChatMessage> {
        val file = fileFor(group)
        if (!file.isFile || file.length() == 0L) return emptyList()
        return runCatching {
            val text = file.readText()
            JSONArray(text).toObjectList(ChatMessage::from)
        }.getOrDefault(emptyList())
    }

    /**
     * Writes the current window for a conversation.
     *
     * Only [MAX_ROWS] newest rows are kept: the cache exists to fill the first screen, and older
     * rows would be re-read from the relay anyway when the user scrolls up.
     */
    fun write(group: String, messages: List<ChatMessage>) {
        if (group.isEmpty() || messages.isEmpty()) return
        runCatching {
            val directory = File(root, "messages").apply { mkdirs() }
            val target = fileFor(group)
            val trimmed = messages.takeLast(MAX_ROWS)
            val array = JSONArray()
            trimmed.forEach { array.put(ChatMessage.toJson(it)) }
            // Write beside the target and rename, so a process death mid-write cannot leave a
            // truncated file that would parse as a shorter history.
            val partial = File(directory, target.name + ".part")
            partial.writeText(array.toString())
            if (target.exists()) target.delete()
            if (!partial.renameTo(target)) {
                partial.copyTo(target, overwrite = true)
                partial.delete()
            }
        }
    }

    /** Drops a conversation's cache, used when the relay no longer allows reading it. */
    fun clear(group: String) {
        runCatching { fileFor(group).delete() }
    }

    /** Drops every conversation, used when the user disconnects from a relay. */
    fun clearAll() {
        runCatching { File(root, "messages").deleteRecursively() }
    }

    private fun sanitize(value: String): String =
        value.map { if (it.isLetterOrDigit() || it == '-' || it == '_' || it == '.') it else '_' }
            .joinToString("")
            .take(80)
            .ifEmpty { "group" }

    companion object {
        /** The desktop keeps a 300-row window in memory; one screen needs far less than that. */
        const val MAX_ROWS = 120
    }
}
