package local.wechatlite.mobile.ui

import android.util.Log

/**
 * Stopwatch for one conversation open.
 *
 * The screen clears its message list on every open, so a slow open looks like a blank chat with no
 * hint about the cause. This records the milestones that separate the candidate causes:
 *
 *  - `activate` / `members` / `history` are network round trips, split apart because they run in
 *    sequence before a single row can be drawn;
 *  - `rows` is the moment the list actually got messages, i.e. the end of the blank period;
 *  - `preview` is the moment the first thumbnail appeared, which is the download cost on top.
 *
 * Times are cumulative milliseconds from the tap, so the gaps between them are the step durations.
 */
class OpenTrace(
    private val group: String,
    private val clock: () -> Long = { System.nanoTime() / 1_000_000 },
) {
    private val started = clock()
    private val marks = LinkedHashMap<String, Long>()

    /** Records a checkpoint once; a later call with the same name is ignored. */
    fun mark(name: String) {
        if (marks.containsKey(name)) return
        marks[name] = clock() - started
    }

    fun elapsed(): Long = clock() - started

    /** Milliseconds from the tap to a checkpoint, or null when it has not been reached yet. */
    fun millisOf(name: String): Long? = marks[name]

    fun has(name: String): Boolean = marks.containsKey(name)

    /** True once the list holds rows: the blank period is over, so the report can be published. */
    fun hasRows(): Boolean = has("rows")

    fun report(): String {
        if (marks.isEmpty()) return ""
        val parts = marks.entries.joinToString(" · ") { (name, at) -> "${labelOf(name)} ${at}ms" }
        return "$group：$parts"
    }

    /**
     * Logs the trace so an attachable `adb logcat -s WeChatLiteTiming` shows the same numbers the
     * settings screen displays. Never includes message content or the connection secret.
     */
    fun publish() {
        Log.i(TAG, report())
    }

    private fun labelOf(name: String): String = when {
        name == "activate" -> "激活"
        name == "members" -> "成员"
        name == "history" -> "历史"
        name == "cache" -> "本地缓存"
        name == "rows" -> "出首屏"
        name == "preview" -> "首图"
        name == "previewAll" -> "预览完"
        // Retries while the relay is still pulling native history: "待微信1", "待微信2", ...
        name.startsWith("wait") -> "待微信" + name.removePrefix("wait")
        else -> name
    }

    companion object {
        const val TAG = "WeChatLiteTiming"
    }
}
