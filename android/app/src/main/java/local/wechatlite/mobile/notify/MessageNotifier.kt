package local.wechatlite.mobile.notify

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import local.wechatlite.mobile.MainActivity
import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.SettingsStore

/**
 * System notifications for new messages, following the per-group level the desktop client uses.
 *
 * Ignored groups never notify. Mentions-only groups notify only for a structured mention of this
 * account. The list marker is intentionally independent of this level, exactly like the desktop UI.
 */
class MessageNotifier(private val context: Context) {
    private val store = SettingsStore(context)

    init {
        createChannel()
    }

    /** Returns true when a notification was actually posted. */
    fun notify(message: ChatMessage, selfId: String, currentGroup: String?, appActive: Boolean): Boolean {
        if (message.group == currentGroup && appActive) {
            // The user is already looking at this conversation; the list marker is enough.
            return false
        }
        val mode = store.notificationMode(message.group)
        if (mode == "ignore") return false
        if (mode == "mentions") {
            val mentioned = message.mentions.any { it.id == selfId && selfId.isNotEmpty() }
            if (!mentioned) return false
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }

        val title = if (message.sender.isBlank()) message.group else message.sender
        val body = message.text.ifEmpty {
            message.media.firstOrNull()?.let { "[${kindLabel(it.kind)}] ${it.name}" } ?: "[附件]"
        }
        val intent = Intent(context, MainActivity::class.java).apply {
            putExtra(EXTRA_GROUP, message.group)
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pending = PendingIntent.getActivity(
            context,
            message.group.hashCode(),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentTitle(title)
            .setContentText(body)
            .setStyle(NotificationCompat.BigTextStyle().bigText(body))
            .setAutoCancel(true)
            // One id per group so a second message replaces the previous one for that conversation
            // instead of stacking an unbounded list.
            .setGroup(GROUP_KEY)
            .setContentIntent(pending)
            .build()
        return runCatching {
            NotificationManagerCompat.from(context).notify(message.group.hashCode(), notification)
            true
        }.getOrDefault(false)
    }

    fun postUnreadBadge(total: Int) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        // A summary carries the group's aggregate; setting it to zero clears the launcher badge.
        if (total <= 0) {
            manager.cancel(SUMMARY_ID)
            return
        }
        val summary = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentTitle("微信轻聊")
            .setContentText("$total 条未读消息")
            .setGroup(GROUP_KEY)
            .setGroupSummary(true)
            .setNumber(total)
            .setSilent(true)
            .build()
        runCatching { manager.notify(SUMMARY_ID, summary) }
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "新消息",
            NotificationManager.IMPORTANCE_DEFAULT,
        ).apply {
            description = "转发群的新消息提醒"
            setShowBadge(true)
        }
        manager.createNotificationChannel(channel)
    }

    private fun kindLabel(kind: String): String = when (kind) {
        "image" -> "图片"
        "gif" -> "动图"
        "video" -> "视频"
        "voice" -> "语音"
        else -> "文件"
    }

    companion object {
        const val EXTRA_GROUP = "group"
        private const val CHANNEL_ID = "messages"
        private const val GROUP_KEY = "local.wechatlite.messages"
        private const val SUMMARY_ID = 1
    }
}
