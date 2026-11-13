package local.wechatlite.mobile.net

import kotlin.math.min
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import local.wechatlite.mobile.data.ServerEvent
import local.wechatlite.mobile.data.SettingsStore

/** What the event stream reports upward: a server event, or a change in connectivity. */
sealed interface StreamUpdate {
    data class Event(val event: ServerEvent) : StreamUpdate
    data object Connected : StreamUpdate
    data class Failed(val message: String, val retryInMillis: Long) : StreamUpdate
}

/**
 * Durable `/api/events` subscription with the desktop client's cursor rules.
 *
 * The last sequence is persisted per token so a reconnect resumes instead of replaying everything;
 * a `reset` event (sent when the requested cursor is ahead of the server) rewinds to zero exactly as
 * `client/transport.cpp` does.
 */
class EventStream(
    private val api: ApiClient,
    private val store: SettingsStore,
    private val identity: String,
) {
    private var cursor: Long = store.cursor(identity)

    fun connect(): Flow<StreamUpdate> = flow {
        var backoff = INITIAL_BACKOFF_MS
        while (true) {
            try {
                api.events(cursor).collect { event ->
                    backoff = INITIAL_BACKOFF_MS
                    when (event) {
                        is ServerEvent.Reset -> {
                            cursor = 0
                            store.setCursor(identity, cursor)
                            emit(StreamUpdate.Event(event))
                        }

                        is ServerEvent.Synced -> {
                            if (event.seq > cursor) {
                                cursor = event.seq
                                store.setCursor(identity, cursor)
                            }
                            emit(StreamUpdate.Connected)
                        }

                        else -> {
                            val seq = event.seq
                            if (seq > 0 && seq <= cursor) {
                                // Already delivered before the reconnect; replay is idempotent but
                                // the desktop client drops these to avoid duplicate notifications.
                                return@collect
                            }
                            if (seq > cursor) {
                                cursor = seq
                                store.setCursor(identity, cursor)
                            }
                            emit(StreamUpdate.Event(event))
                        }
                    }
                }
                // The server closed cleanly; reconnect from the persisted cursor.
                emit(StreamUpdate.Failed("事件连接已断开，正在重连", backoff))
            } catch (e: Exception) {
                val message = (e as? ApiException)?.message ?: "事件连接失败，正在重连"
                emit(StreamUpdate.Failed(message, backoff))
            }
            delay(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_MS)
        }
    }

    companion object {
        private const val INITIAL_BACKOFF_MS = 1_000L
        private const val MAX_BACKOFF_MS = 30_000L
    }
}
