package local.wechatlite.mobile.data

import android.content.Context
import org.json.JSONObject

/**
 * Persists the invitation, the event cursor and per-group notification choices.
 *
 * The token never leaves the app's private preferences; it is not written to logs or exports.
 */
class SettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("wechatlite", Context.MODE_PRIVATE)

    var connectionJson: String?
        get() = prefs.getString(KEY_CONNECTION, null)
        set(value) {
            prefs.edit().apply {
                if (value == null) remove(KEY_CONNECTION) else putString(KEY_CONNECTION, value)
            }.apply()
        }

    var displayName: String
        get() = prefs.getString(KEY_DISPLAY_NAME, "") ?: ""
        set(value) = prefs.edit().putString(KEY_DISPLAY_NAME, value).apply()

    /**
     * Cursors are keyed by a digest of the token exactly like the desktop client, so two servers
     * with different keys never share a replay position.
     */
    private fun cursorKey(identity: String) = "cursor_$identity"

    fun cursor(identity: String): Long = prefs.getLong(cursorKey(identity), -1L)

    fun setCursor(identity: String, value: Long) {
        prefs.edit().putLong(cursorKey(identity), value).apply()
    }

    /** Notification level per group: "all", "mentions" or "ignore". */
    fun notificationMode(group: String): String = prefs.getString("notify_$group", "all") ?: "all"

    fun setNotificationMode(group: String, mode: String) {
        prefs.edit().putString("notify_$group", mode).apply()
    }

    fun pinned(group: String): Boolean = prefs.getBoolean("pinned_$group", false)

    fun setPinned(group: String, value: Boolean) {
        prefs.edit().putBoolean("pinned_$group", value).apply()
    }

    fun themeMode(): String = prefs.getString("theme", "system") ?: "system"

    fun setThemeMode(value: String) {
        prefs.edit().putString("theme", value).apply()
    }

    /**
     * A stable per-install identifier sent as `client_id`. The server treats it as a display and
     * deduplication field, never as authentication: the bearer key remains the identity.
     */
    fun clientId(): String {
        prefs.getString(KEY_CLIENT_ID, null)?.let { return it }
        val created = "android-" + java.util.UUID.randomUUID().toString().replace("-", "").take(20)
        prefs.edit().putString(KEY_CLIENT_ID, created).apply()
        return created
    }

    /**
     * Whether the one-time purge of message caches written by builds before 0.1.8 has run.
     *
     * Those builds could merge a slow conversation's page into the conversation opened after it, and
     * the mixed list was then written to that group's cache file. The files look valid, so they have
     * to be discarded once instead of being migrated.
     */
    fun messageCachePurged(): Boolean = prefs.getBoolean(KEY_CACHE_PURGED, false)

    fun markMessageCachePurged() {
        prefs.edit().putBoolean(KEY_CACHE_PURGED, true).apply()
    }

    private companion object {
        const val KEY_CONNECTION = "connection"
        const val KEY_DISPLAY_NAME = "display_name"
        const val KEY_CLIENT_ID = "client_id"
        const val KEY_CACHE_PURGED = "message_cache_purged_v2"
    }
}
