package local.wechatlite.mobile.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import local.wechatlite.mobile.data.SettingsStore

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    state: ChatUiState,
    onClose: () -> Unit,
    onDisconnect: () -> Unit,
    onReconnect: () -> Unit,
) {
    val context = LocalContext.current
    val store = remember { SettingsStore(context) }
    var displayName by remember { mutableStateOf(store.displayName) }
    var theme by remember { mutableStateOf(store.themeMode()) }

    Scaffold(
        topBar = {
            TopAppBar(
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                title = { Text("设置") },
            )
        },
    ) { padding ->
        Column(
            Modifier.fillMaxSize().padding(padding).verticalScroll(rememberScrollState()),
        ) {
            SectionTitle("连接")
            InfoRow(
                "服务器",
                when (val connection = state.connection) {
                    null -> "未配置"
                    is local.wechatlite.mobile.data.ConnectionConfig.Direct -> connection.serverUrl
                    is local.wechatlite.mobile.data.ConnectionConfig.P2p -> "p2p://${connection.serverId}"
                },
            )
            InfoRow(
                "模式",
                if (state.connection is local.wechatlite.mobile.data.ConnectionConfig.P2p) {
                    "自动连接（直连优先）"
                } else {
                    "直接地址"
                },
            )
            InfoRow("当前身份", state.status?.clientName?.ifEmpty { "—" } ?: "—")
            InfoRow(
                "服务状态",
                when {
                    !state.connected -> "未连接"
                    state.status?.ready != true -> "已连接，等待微信"
                    else -> "正常"
                },
            )
            InfoRow("发送权限", if (state.status?.canSend == true) "允许" else "不允许")
            InfoRow("下载权限", if (state.status?.canDownload == true) "允许" else "不允许")
            InfoRow("在线客户端", state.status?.onlineClients?.takeIf { it > 0 }?.toString() ?: "—")
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
                TextButton(onClick = onReconnect) { Text("重新连接") }
                TextButton(onClick = onDisconnect) { Text("断开并更换连接") }
            }

            HorizontalDivider()
            SectionTitle("身份")
            InfoRow("发送显示名", displayName.ifBlank { "手机端" })
            TextButton(
                onClick = {
                    // Cycling a suffix keeps the field editable without a system keyboard dialog here.
                    displayName = if (displayName.isBlank()) "手机端" else ""
                    store.displayName = displayName
                },
                modifier = Modifier.padding(horizontal = 16.dp),
            ) { Text("切换显示名") }

            HorizontalDivider()
            SectionTitle("外观")
            listOf("system" to "跟随系统", "light" to "浅色", "dark" to "深色").forEach { (value, label) ->
                Row(
                    Modifier.fillMaxWidth().clickable {
                        theme = value
                        store.setThemeMode(value)
                    }.padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(label, modifier = Modifier.weight(1f))
                    if (theme == value) Text("✓", color = MaterialTheme.colorScheme.primary)
                }
            }

            HorizontalDivider()
            SectionTitle("会话列表")
            InfoRow("会话数", state.groups.size.toString())
            InfoRow(
                "有最新消息的会话",
                state.groups.count { normalizePreview(it.preview).isNotEmpty() }.toString(),
            )
            InfoRow("摘要来源", state.summarySource.ifEmpty { "尚未读取群列表" })
            InfoRow(
                "打开耗时",
                state.openTiming.ifEmpty { "尚未打开任何会话" },
            )

            HorizontalDivider()
            SectionTitle("关于")
            InfoRow("版本", buildVersionLabel())
            Text(
                "微信轻聊 Android 客户端。服务器继续运行在登录微信的 Windows 电脑上；本客户端不包含微信适配器，也不保存聊天数据库。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 8.dp),
    )
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        // `weight` keeps a long value (the open-timing breakdown) wrapping inside the row instead of
        // being squeezed into a one-character-wide column next to the label.
        Text(
            value,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = androidx.compose.ui.text.style.TextAlign.End,
            modifier = Modifier.padding(start = 12.dp).weight(1f),
        )
    }
}

/** Reads the persisted theme without composing a screen, so the root can pick a scheme first. */
fun themeModeOf(context: android.content.Context): String = SettingsStore(context).themeMode()

/** Version plus the build stamp, so a screenshot tells one rebuild from another. */
private fun buildVersionLabel(): String =
    "v${local.wechatlite.mobile.BuildConfig.VERSION_NAME} (${local.wechatlite.mobile.BuildConfig.BUILD_STAMP})"
