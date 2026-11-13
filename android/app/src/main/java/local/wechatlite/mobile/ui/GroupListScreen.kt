package local.wechatlite.mobile.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import local.wechatlite.mobile.data.Group
import local.wechatlite.mobile.data.ServerStatus

/**
 * Conversation list, mirroring the desktop sidebar: pinned conversations on top, then by newest
 * message, each row showing that message's summary, its clock and a red unread pill.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GroupListScreen(
    state: ChatUiState,
    onOpen: (Group) -> Unit,
    onRefresh: () -> Unit,
    onSearch: () -> Unit,
    onSettings: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("微信轻聊", fontWeight = FontWeight.SemiBold)
                        Text(
                            connectionLabel(state),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                },
                actions = {
                    IconButton(onClick = onSearch) { Icon(Icons.Default.Search, contentDescription = "搜索聊天记录") }
                    IconButton(onClick = onRefresh) { Icon(Icons.Default.Refresh, contentDescription = "刷新") }
                    IconButton(onClick = { menuOpen = true }) { Icon(Icons.Default.MoreVert, contentDescription = "更多") }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(text = { Text("设置") }, onClick = { menuOpen = false; onSettings() })
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when {
                state.connecting -> CenteredProgress("正在连接服务器…")
                state.groups.isEmpty() -> EmptyGroups(state)
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    items(state.groups, key = { it.id }) { group ->
                        GroupRow(group = group, onClick = { onOpen(group) })
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))
                    }
                }
            }
        }
    }
}

private fun connectionLabel(state: ChatUiState): String = when {
    !state.connected -> "未连接"
    state.status?.ready != true -> "服务端已连接，等待微信"
    else -> "已连接 · ${state.status?.clientName.orEmpty()}"
}

@Composable
private fun GroupRow(group: Group, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box {
            Avatar(name = group.name, avatar = group.avatar, size = 46.dp, identity = group.id)
            // The desktop badge hangs off the avatar's top-right corner (`right-6, top-5`).
            if (group.unread > 0) {
                UnreadBadge(group.unread, Modifier.align(Alignment.TopEnd).offset(x = 5.dp, y = (-5).dp))
            }
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    group.name,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                if (group.pinned) {
                    Spacer(Modifier.width(6.dp))
                    Text("置顶", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                }
                if (group.timestamp > 0) {
                    Spacer(Modifier.width(8.dp))
                    Text(
                        clockLabel(group.timestamp),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
            }
            Spacer(Modifier.height(2.dp))
            // The server stores the message text verbatim, so a preview that starts with a line
            // break would draw an empty line. Normalise first, then decide it needs the fallback.
            Text(
                normalizePreview(group.preview).ifEmpty { "暂无消息" },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

/** Red pill drawn over the avatar, mirroring the desktop sidebar's 15/20 px badge. */
@Composable
private fun UnreadBadge(count: Int, modifier: Modifier = Modifier) {
    // The pill is a fixed 15 dp shape, exactly like the desktop's 15 px badge, so its label must
    // not grow with the system font size: a larger digit would spill out of the circle and read as
    // off-centre. The desktop draws this number at a fixed 10 px too.
    val density = LocalDensity.current
    Box(
        modifier = modifier
            .size(width = if (count > 9) 20.dp else 15.dp, height = 15.dp)
            .clip(CircleShape)
            .background(BadgeFill),
        contentAlignment = Alignment.Center,
    ) {
        CompositionLocalProvider(LocalDensity provides Density(density.density, fontScale = 1f)) {
            Text(
                text = if (count > 99) "99+" else count.toString(),
                color = Color.White,
                style = TextStyle(
                    fontSize = 10.sp,
                    lineHeight = 10.sp,
                    textAlign = TextAlign.Center,
                    // Android reserves extra room above the cap height for ascenders. For one line
                    // of digits that room stays below the visual middle, so the number reads as
                    // sitting low in the circle. Qt's AlignCenter has no such padding.
                    platformStyle = PlatformTextStyle(includeFontPadding = false),
                ),
                maxLines = 1,
                softWrap = false,
            )
        }
    }
}

/** Clock shown on the right of a row, using the desktop sidebar's `HH:mm` format. */
private fun clockLabel(timestamp: Long): String =
    java.time.Instant.ofEpochMilli(timestamp)
        .atZone(java.time.ZoneId.systemDefault())
        .format(java.time.format.DateTimeFormatter.ofPattern("HH:mm"))

private val BadgeFill = Color(0xFFFF5059)

/**
 * Whether the app is drawing its dark palette. The theme setting can disagree with the system one,
 * so widgets that need literal colours read this instead of `isSystemInDarkTheme`.
 */
val LocalDarkTheme = staticCompositionLocalOf { false }

/** Placeholder palette, copied from the desktop `chatAvatar` so a name keeps a stable colour. */
private val AvatarFills = listOf(
    Color(0xFFCBD9CC), Color(0xFFDAD2C6), Color(0xFFD8D8DF),
    Color(0xFFB9C9D4), Color(0xFFD9C5BF), Color(0xFFCAD5BB),
)
private val AvatarFillsDark = listOf(
    Color(0xFF4A5B4E), Color(0xFF5D554A), Color(0xFF535360),
    Color(0xFF455866), Color(0xFF624E49), Color(0xFF505E46),
)

/**
 * Round avatar with an initial fallback, matching the desktop client's bounded avatar cache.
 *
 * `identity` seeds the placeholder colour the way the desktop hashes the sender id: without it
 * every member without a picture would look identical, which also made the chat look as if no
 * avatar were drawn at all.
 */
@Composable
fun Avatar(
    name: String,
    avatar: String,
    size: androidx.compose.ui.unit.Dp,
    identity: String = name,
) {
    val bitmap = remember(avatar) { decodeBase64Avatar(avatar) }
    val dark = LocalDarkTheme.current
    val fill = remember(identity, dark) {
        val palette = if (dark) AvatarFillsDark else AvatarFills
        palette[(identity.hashCode().toLong() and 0x7fffffff).rem(palette.size).toInt()]
    }
    Box(
        Modifier
            .size(size)
            .clip(CircleShape)
            .background(if (bitmap != null) Color.Transparent else fill),
        contentAlignment = Alignment.Center,
    ) {
        if (bitmap != null) {
            androidx.compose.foundation.Image(
                bitmap = bitmap,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Text(
                name.take(1).ifEmpty { "?" },
                color = if (dark) Color(0xFFDCE5DF) else Color(0xFF52605B),
                // Same proportion as the desktop: 47 % of the square.
                fontSize = (size.value * 0.47f).sp,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun EmptyGroups(state: ChatUiState) {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("没有可用的群", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Text(
            state.status?.let { describeEmptyGroups(it) } ?: "请确认服务端已启动并勾选转发群。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun describeEmptyGroups(status: ServerStatus): String = when {
    !status.ready -> "服务端已连接，但本机微信尚未就绪。"
    status.allowedGroups.isEmpty() -> "当前客户端没有获得任何群的读取权限。"
    else -> "全局转发群与该客户端的授权群没有交集。"
}

@Composable
fun CenteredProgress(text: String) {
    Column(
        Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        CircularProgressIndicator()
        Spacer(Modifier.height(12.dp))
        Text(text, style = MaterialTheme.typography.bodySmall)
    }
}

/**
 * Decodes the server's Base64 JPEG avatar. The server caps avatars at 64 pixels, so the decode is
 * cheap; anything malformed falls back to the initial instead of failing the row.
 */
private fun decodeBase64Avatar(value: String): androidx.compose.ui.graphics.ImageBitmap? {
    if (value.isEmpty() || value.length > 64 * 1024) return null
    return runCatching {
        val bytes = android.util.Base64.decode(value, android.util.Base64.DEFAULT)
        android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
    }.getOrNull()
}
