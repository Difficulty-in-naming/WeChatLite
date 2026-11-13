package local.wechatlite.mobile.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import local.wechatlite.mobile.data.ChatMessage

/** Read-only group announcement. Distinguishes "not set" from "not readable" like the desktop UI. */
@Composable
fun AnnouncementSheet(state: ChatUiState, onClose: () -> Unit) {
    val announcement = state.announcement
    AlertDialog(
        onDismissRequest = onClose,
        confirmButton = { TextButton(onClick = onClose) { Text("关闭") } },
        title = { Text("群公告", fontWeight = FontWeight.SemiBold) },
        text = {
            Column(Modifier.heightIn(max = 420.dp)) {
                when {
                    announcement == null -> Row(
                        Modifier.fillMaxWidth().padding(vertical = 24.dp),
                        horizontalArrangement = Arrangement.Center,
                    ) {
                        CircularProgressIndicator(Modifier.height(24.dp))
                    }

                    !announcement.available -> Text(
                        if (announcement.pending) "正在读取群公告…" else "暂不可读，请稍后重试。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )

                    !announcement.set -> Text(
                        "尚未设置群公告。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )

                    else -> Column(Modifier.verticalScroll(rememberScrollState())) {
                        Row {
                            Text(announcement.author, style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.Medium)
                            Spacer(Modifier.padding(horizontal = 6.dp))
                            announcement.updatedAt?.let {
                                Text(
                                    formatDateTime(it),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                        Spacer(Modifier.height(10.dp))
                        Text(announcement.text, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        },
    )
}

/**
 * Read-only merged forward card. Items keep their source order and reference the enclosing
 * message's media array by index, exactly as the adapter publishes them.
 */
@Composable
fun RecordSheet(message: ChatMessage, onClose: () -> Unit) {
    val record = message.record ?: return
    AlertDialog(
        onDismissRequest = onClose,
        confirmButton = { TextButton(onClick = onClose) { Text("关闭") } },
        title = {
            Column {
                Text(record.title.ifEmpty { "聊天记录" }, fontWeight = FontWeight.SemiBold, maxLines = 2, overflow = TextOverflow.Ellipsis)
                if (record.pending) {
                    Text(
                        "部分附件仍在下载",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        text = {
            Column(Modifier.heightIn(max = 460.dp).verticalScroll(rememberScrollState())) {
                record.items.forEach { item ->
                    Surface(
                        color = MaterialTheme.colorScheme.surfaceVariant,
                        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
                    ) {
                        Column(Modifier.padding(10.dp)) {
                            Text(
                                item.sender,
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.primary,
                            )
                            if (item.text.isNotEmpty()) {
                                Text(item.text, style = MaterialTheme.typography.bodySmall)
                            }
                            item.media.forEach { index ->
                                message.media.getOrNull(index)?.let { media ->
                                    Text(
                                        "[${mediaKind(media.kind)}] ${media.name}",
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                        }
                    }
                }
            }
        },
    )
}

private fun mediaKind(kind: String): String = when (kind) {
    "image" -> "图片"
    "gif" -> "动图"
    "video" -> "视频"
    "voice" -> "语音"
    else -> "文件"
}

private fun formatDateTime(timestamp: Long): String =
    SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(Date(timestamp))
