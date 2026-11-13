package local.wechatlite.mobile.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.AttachFile
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.text.SimpleDateFormat
import java.util.Date
import java.io.File
import java.util.Locale
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.Group
import local.wechatlite.mobile.data.Media
import local.wechatlite.mobile.data.Member

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ChatScreen(
    state: ChatUiState,
    onBack: () -> Unit,
    onSend: () -> Unit,
    onDraftChange: (String) -> Unit,
    onLoadOlder: () -> Unit,
    onAttach: () -> Unit,
    onRemoveAttachment: (android.net.Uri) -> Unit,
    onPickMention: (Member) -> Unit,
    onReply: (ChatMessage?) -> Unit,
    onRecall: (ChatMessage) -> Unit,
    onOpenAnnouncement: () -> Unit,
    onCloseAnnouncement: () -> Unit,
    onOpenRecords: () -> Unit,
    onTogglePin: () -> Unit,
    onNotificationMode: (String) -> Unit,
    onOpenMedia: (ChatMessage, Media) -> Unit,
    onPreviewNeeded: (List<Media>) -> Unit,
    onOpenRecord: (ChatMessage) -> Unit,
    onCloseRecord: () -> Unit,
    onSettings: () -> Unit,
) {
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    var menuOpen by remember { mutableStateOf(false) }
    var actionTarget by remember { mutableStateOf<ChatMessage?>(null) }
    val group = state.currentGroup ?: return

    // The newest page arrives after the screen opens, so waiting on `group.id` alone would run
    // once against an empty list and never position the view again.
    var positioned by remember(group.id) { mutableStateOf(false) }
    LaunchedEffect(group.id, state.messages.isNotEmpty()) {
        if (!positioned && state.messages.isNotEmpty()) {
            // Row 0 is the history loader, so the newest message is at index `messages.size`.
            listState.scrollToItem(state.messages.size)
            positioned = true
        }
    }

    // Fetch older pages when the user scrolls near the top. Wait until the first page has been
    // positioned at the bottom, otherwise the initial index 0 would immediately page backwards.
    val shouldLoadOlder by remember {
        derivedStateOf {
            positioned && state.messages.isNotEmpty() && listState.firstVisibleItemIndex <= 2
        }
    }
    LaunchedEffect(shouldLoadOlder, state.hasOlder, state.loadingOlder) {
        if (shouldLoadOlder && state.hasOlder && !state.loadingOlder) onLoadOlder()
    }

    // Only the rows on screen ask for thumbnails, exactly like the desktop client's
    // `visibleThumbnails()`. The snapshot re-runs while the user scrolls or when messages arrive.
    // `canDownload` is a key as well: when the server grants permission after the page is drawn, the
    // visible rows must ask again instead of keeping their "点击下载" cards.
    val messages = state.messages
    LaunchedEffect(listState, messages, state.canDownload) {
        snapshotFlow {
            val info = listState.layoutInfo
            val visible = info.visibleItemsInfo
            if (visible.isEmpty()) return@snapshotFlow emptyList<Media>()
            // Index 0 is the history loader, so a message sits at its index + 1.
            val first = maxOf(0, visible.first().index - 1)
            val last = minOf(messages.lastIndex, visible.last().index - 1)
            if (first > last) {
                emptyList()
            } else {
                // A recalled row draws no attachment, so it must not trigger a download either.
                messages.subList(first, last + 1).filter { !it.isRecalled }.flatMap { it.media }
            }
        }.distinctUntilChanged().collect { onPreviewNeeded(it) }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                title = {
                    Column {
                        Text(group.name, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            buildString {
                                append("${state.members.size} 位成员")
                                if (!state.connected) append(" · 未连接")
                                else if (state.status?.ready != true) append(" · 等待微信")
                            },
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                },
                actions = {
                    IconButton(onClick = { menuOpen = true }) { Icon(Icons.Default.MoreVert, contentDescription = "更多") }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(text = { Text("群公告") }, onClick = { menuOpen = false; onOpenAnnouncement() })
                        DropdownMenuItem(text = { Text("聊天记录") }, onClick = { menuOpen = false; onOpenRecords() })
                        DropdownMenuItem(
                            text = { Text(if (state.pinned) "取消置顶" else "置顶") },
                            onClick = { menuOpen = false; onTogglePin() },
                        )
                        HorizontalDivider()
                        DropdownMenuItem(
                            text = { Text(if (state.notificationMode == "ignore") "• 忽略通知" else "忽略通知") },
                            onClick = { menuOpen = false; onNotificationMode("ignore") },
                        )
                        DropdownMenuItem(
                            text = { Text(if (state.notificationMode == "mentions") "• 仅被 @ 时通知" else "仅被 @ 时通知") },
                            onClick = { menuOpen = false; onNotificationMode("mentions") },
                        )
                        DropdownMenuItem(
                            text = { Text(if (state.notificationMode == "all") "• 所有新消息通知" else "所有新消息通知") },
                            onClick = { menuOpen = false; onNotificationMode("all") },
                        )
                        HorizontalDivider()
                        DropdownMenuItem(text = { Text("设置") }, onClick = { menuOpen = false; onSettings() })
                    }
                },
            )
        },
        bottomBar = {
            Composer(
                state = state,
                onSend = onSend,
                onDraftChange = onDraftChange,
                onAttach = onAttach,
                onRemoveAttachment = onRemoveAttachment,
                onPickMention = onPickMention,
                onReply = onReply,
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when {
                state.messages.isEmpty() && state.loadingOlder -> CenteredProgress("正在读取消息…")
                state.messages.isEmpty() -> Column(
                    Modifier.fillMaxSize().padding(32.dp),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("还没有消息", style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.height(6.dp))
                    Text(
                        if (state.status?.ready == true) "该群暂无已转发的消息。" else "服务端尚未连接本机微信。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                else -> LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp),
                ) {
                    item(key = "history-loader") {
                        if (state.loadingOlder) {
                            Box(Modifier.fillMaxWidth().padding(8.dp), contentAlignment = Alignment.Center) {
                                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                            }
                        } else if (!state.hasOlder) {
                            Text(
                                "没有更早的消息",
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.fillMaxWidth().padding(12.dp),
                                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                            )
                        }
                    }
                    items(state.messages.size) { index ->
                        val message = state.messages[index]
                        MessageRow(
                            message = message,
                            showSender = index == 0 ||
                                state.messages[index - 1].sender != message.sender ||
                                message.timestamp - state.messages[index - 1].timestamp > 5 * 60_000,
                            recallSupported = state.recallSupported,
                            onLongPress = { actionTarget = message },
                            onOpenMedia = { onOpenMedia(message, it) },
                            mediaFiles = state.mediaFiles,
                            mediaPosters = state.mediaPosters,
                            mediaLoading = state.mediaLoading,
                            onOpenRecord = { onOpenRecord(message) },
                        )
                    }
                }
            }

            state.replyTo?.let { reply ->
                // A compact reply banner sits above the composer area without stealing focus.
                Surface(
                    modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth(),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                ) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("回复 ${reply.sender}", style = MaterialTheme.typography.labelSmall)
                            Text(reply.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        IconButton(onClick = { onReply(null) }) { Icon(Icons.Default.Close, contentDescription = "取消回复") }
                    }
                }
            }
        }

        actionTarget?.let { target ->
            MessageActionsSheet(
                message = target,
                recallSupported = state.recallSupported,
                onDismiss = { actionTarget = null },
                onReply = { actionTarget = null; onReply(target) },
                onRecall = { actionTarget = null; onRecall(target) },
            )
        }

        if (state.announcementOpen) {
            AnnouncementSheet(state = state, onClose = onCloseAnnouncement)
        }
        state.recordMessage?.let { record -> RecordSheet(message = record, onClose = onCloseRecord) }
    }
}

@Composable
private fun MessageRow(
    message: ChatMessage,
    showSender: Boolean,
    recallSupported: Boolean,
    onLongPress: () -> Unit,
    onOpenMedia: (Media) -> Unit,
    mediaFiles: Map<String, File>,
    mediaPosters: Map<String, File>,
    mediaLoading: Set<String>,
    onOpenRecord: () -> Unit,
) {
    val mine = message.outgoing
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 3.dp),
        horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start,
    ) {
        // The desktop draws the sender's avatar beside every bubble — left for received, right for
        // outgoing, top-aligned with the bubble (`MessageDelegate::paint`). `message.avatar` is the
        // sender's picture for both directions, so outgoing rows show the account itself.
        if (!mine) {
            Avatar(name = message.sender, avatar = message.avatar, size = 36.dp, identity = message.senderId)
            Spacer(Modifier.width(8.dp))
        }
        Box(
            modifier = Modifier
                // `fill = false` keeps a short bubble narrow; the width still shrinks on a small
                // screen so the avatar never gets pushed off the row.
                .weight(1f, fill = false)
                .widthIn(max = 300.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(if (mine) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant)
                .combinedClickable(onClick = {}, onLongClick = onLongPress)
                .padding(10.dp),
        ) {
            Column {
                if (showSender && !mine) {
                    Text(
                        message.sender,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(bottom = 3.dp),
                    )
                }
                message.quote?.let { quote ->
                    Surface(
                        color = MaterialTheme.colorScheme.surface.copy(alpha = 0.5f),
                        shape = RoundedCornerShape(6.dp),
                        modifier = Modifier.fillMaxWidth().padding(bottom = 6.dp),
                    ) {
                        Column(Modifier.padding(6.dp)) {
                            Text(quote.sender, style = MaterialTheme.typography.labelSmall)
                            Text(
                                if (quote.unavailable) "[原文已撤回]" else quote.text,
                                style = MaterialTheme.typography.bodySmall,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }

                if (message.isRecalled) {
                    Text(
                        if (mine) "你撤回了一条消息" else "${message.sender} 撤回了一条消息",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                } else {
                    if (message.text.isNotEmpty()) {
                        Text(message.text, style = MaterialTheme.typography.bodyMedium)
                    }
                    message.media.forEach { media ->
                        Spacer(Modifier.height(4.dp))
                        AttachmentPreview(
                            media = media,
                            file = if (media.isVideo) null else mediaFiles[media.id],
                            poster = if (media.isVideo) mediaPosters[media.id] else null,
                            loading = media.id in mediaLoading,
                            onOpen = { onOpenMedia(media) },
                            onLongPress = onLongPress,
                        )
                    }
                    message.record?.let { record ->
                        Spacer(Modifier.height(4.dp))
                        RecordCard(title = record.title, count = record.items.size, onOpen = onOpenRecord)
                    }
                }

                Row(
                    modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (message.recallStatus == "failed" || message.recallStatus == "uncertain") {
                        Text(
                            if (message.recallStatus == "uncertain") "撤回状态不确定" else "撤回失败",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.error,
                        )
                        Spacer(Modifier.width(6.dp))
                    }
                    Text(
                        statusLabel(message),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        if (mine) {
            Spacer(Modifier.width(8.dp))
            Avatar(name = message.sender, avatar = message.avatar, size = 36.dp, identity = message.senderId)
        }
    }
}

private fun statusLabel(message: ChatMessage): String = when {
    message.status == "sending" -> "发送中…"
    message.status == "queued" -> "排队中"
    message.status == "failed" -> "发送失败"
    message.status == "uncertain" -> "结果不确定"
    else -> formatTime(message.timestamp)
}

private fun formatTime(timestamp: Long): String {
    if (timestamp <= 0) return ""
    val now = System.currentTimeMillis()
    val sameDay = SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date(timestamp)) ==
        SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date(now))
    val pattern = if (sameDay) "HH:mm" else "MM-dd HH:mm"
    return SimpleDateFormat(pattern, Locale.getDefault()).format(Date(timestamp))
}

private fun Modifier.combinedClickableCompat(onClick: () -> Unit): Modifier =
    this.clickable(onClick = onClick)

@Composable
fun RecordCard(title: String, count: Int, onOpen: () -> Unit) {
    Surface(
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(8.dp),
        tonalElevation = 1.dp,
        modifier = Modifier.fillMaxWidth().combinedClickableCompat(onOpen),
    ) {
        Column(Modifier.padding(10.dp)) {
            Text(title.ifEmpty { "聊天记录" }, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
            Text(
                "$count 条内容 · 点击查看",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun Composer(
    state: ChatUiState,
    onSend: () -> Unit,
    onDraftChange: (String) -> Unit,
    onAttach: () -> Unit,
    onRemoveAttachment: (android.net.Uri) -> Unit,
    onPickMention: (Member) -> Unit,
    onReply: (ChatMessage?) -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.surface, tonalElevation = 3.dp) {
        Column(Modifier.imePadding().navigationBarsPadding().padding(8.dp)) {
            if (state.mentionResults.isNotEmpty()) {
                Surface(
                    color = MaterialTheme.colorScheme.surfaceVariant,
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.fillMaxWidth().heightIn(max = 180.dp),
                ) {
                    Column {
                        state.mentionResults.forEach { member ->
                            Text(
                                "${member.name}${if (member.remark.isNotEmpty()) "（${member.remark}）" else ""}",
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .combinedClickableCompat { onPickMention(member) }
                                    .padding(horizontal = 12.dp, vertical = 8.dp),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    }
                }
                Spacer(Modifier.height(6.dp))
            }

            if (state.attachments.isNotEmpty()) {
                Row(Modifier.fillMaxWidth().padding(bottom = 6.dp)) {
                    state.attachments.forEach { attachment ->
                        Surface(
                            color = MaterialTheme.colorScheme.surfaceVariant,
                            shape = RoundedCornerShape(6.dp),
                            modifier = Modifier.padding(end = 6.dp),
                        ) {
                            Row(Modifier.padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    attachment.name,
                                    style = MaterialTheme.typography.labelSmall,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.widthIn(max = 110.dp),
                                )
                                IconButton(onClick = { onRemoveAttachment(attachment.uri) }, modifier = Modifier.size(20.dp)) {
                                    Icon(Icons.Default.Close, contentDescription = "移除")
                                }
                            }
                        }
                    }
                }
            }

            Row(verticalAlignment = Alignment.Bottom) {
                IconButton(onClick = onAttach, enabled = state.canSend) {
                    Icon(Icons.Default.AttachFile, contentDescription = "添加附件")
                }
                TextField(
                    value = state.draft,
                    onValueChange = onDraftChange,
                    modifier = Modifier.weight(1f),
                    placeholder = {
                        Text(
                            when {
                                !state.connected -> "连接已断开"
                                !state.canSend -> "当前客户端没有发送权限"
                                state.status?.ready != true -> "等待服务端微信连接"
                                else -> "输入消息"
                            },
                        )
                    },
                    enabled = state.canSend,
                    maxLines = 5,
                    shape = RoundedCornerShape(18.dp),
                    colors = TextFieldDefaults.colors(
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent,
                    ),
                )
                IconButton(
                    onClick = onSend,
                    enabled = state.canSend && !state.sending &&
                        (state.draft.isNotBlank() || state.attachments.isNotEmpty()),
                ) {
                    if (state.sending) {
                        CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    } else {
                        Icon(Icons.AutoMirrored.Filled.Send, contentDescription = "发送")
                    }
                }
            }
        }
    }
}

/** Long-press actions. Recall is offered only when the server reports support and the window holds. */
@Composable
private fun MessageActionsSheet(
    message: ChatMessage,
    recallSupported: Boolean,
    onDismiss: () -> Unit,
    onReply: () -> Unit,
    onRecall: () -> Unit,
) {
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = {
            Row {
                TextButton(onClick = onReply) { Text("引用") }
                if (message.outgoing && !message.isRecalled) {
                    TextButton(onClick = onRecall, enabled = recallSupported && message.canRecall()) {
                        Text("撤回")
                    }
                }
                TextButton(onClick = onDismiss) { Text("关闭") }
            }
        },
        title = { Text(message.sender, style = MaterialTheme.typography.titleSmall) },
        text = {
            Column {
                Text(message.text.take(300), style = MaterialTheme.typography.bodySmall)
                if (message.outgoing && recallSupported && !message.canRecall() && !message.isRecalled) {
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "该消息已超过 180 秒或尚未确认，不能撤回。",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (!recallSupported) {
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "当前微信版本的后台撤回暂不可用。",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
    )
}
