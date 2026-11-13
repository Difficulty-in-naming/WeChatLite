package local.wechatlite.mobile

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.ContextCompat
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import java.io.File
import java.util.Locale
import kotlinx.coroutines.launch
import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.Media
import local.wechatlite.mobile.ui.ChatScreen
import local.wechatlite.mobile.ui.ChatViewModel
import local.wechatlite.mobile.ui.ConnectScreen
import local.wechatlite.mobile.ui.GroupListScreen
import local.wechatlite.mobile.ui.LocalDarkTheme
import local.wechatlite.mobile.ui.RecordSearchScreen
import local.wechatlite.mobile.ui.SettingsScreen
import local.wechatlite.mobile.notify.MessageNotifier
import local.wechatlite.mobile.ui.themeModeOf

class MainActivity : ComponentActivity() {
    private val viewModel: ChatViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Android 13+ asks for notification permission at runtime; without it the app still works,
        // it just cannot post new-message notifications.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            registerForActivityResult(ActivityResultContracts.RequestPermission()) { }
                .launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        // A launch from a notification opens the conversation it belongs to.
        intent?.getStringExtra(MessageNotifier.EXTRA_GROUP)?.let { group ->
            viewModel.pendingGroupId = group
        }

        setContent {
            val state by viewModel.state.collectAsState()
            val dark = when (themeModeOf(this)) {
                "dark" -> true
                "light" -> false
                else -> isSystemInDarkTheme()
            }
            MaterialTheme(colorScheme = if (dark) DarkScheme else LightScheme) {
                CompositionLocalProvider(LocalDarkTheme provides dark) {
                    Surface(Modifier.fillMaxSize()) { AppRoot(viewModel, state) }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        viewModel.setForeground(true)
        // Coming back to the foreground clears the marker of the conversation that is on screen,
        // matching the desktop client's `markConversationRead` on window activation. Other
        // conversations keep their badges.
        viewModel.markConversationRead()
    }

    override fun onPause() {
        viewModel.setForeground(false)
        super.onPause()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.getStringExtra(MessageNotifier.EXTRA_GROUP)?.let { viewModel.openGroupById(it) }
    }
}

private val Green = Color(0xFF087F52)

private val LightScheme = lightColorScheme(
    primary = Green,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFD3F0E4),
    onPrimaryContainer = Color(0xFF00281A),
    secondaryContainer = Color(0xFFE3E8E4),
    onSecondaryContainer = Color(0xFF1B211D),
)

private val DarkScheme = darkColorScheme(
    primary = Color(0xFF7FD8B4),
    onPrimary = Color(0xFF003824),
    primaryContainer = Color(0xFF005236),
    onPrimaryContainer = Color(0xFF9BF5CE),
    secondaryContainer = Color(0xFF2A332C),
    onSecondaryContainer = Color(0xFFDDE5DD),
)

@Composable
private fun AppRoot(viewModel: ChatViewModel, state: local.wechatlite.mobile.ui.ChatUiState) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    var showSettings by remember { mutableStateOf(false) }
    var showSearch by remember { mutableStateOf(false) }

    val attachmentPicker = androidx.activity.compose.rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments(),
    ) { uris ->
        uris.forEach { uri -> viewModel.addAttachment(uri, displayName(context, uri), sizeOf(context, uri)) }
    }

    LaunchedEffect(state.error) {
        state.error?.let { snackbar.showSnackbar(it); viewModel.dismissError() }
    }
    LaunchedEffect(state.notice) {
        state.notice?.let { snackbar.showSnackbar(it); viewModel.dismissNotice() }
    }

    // The system back gesture only navigates inside the app: it never finishes the activity.
    // There is deliberately no "exit app" path here — the user closes the app from the launcher
    // or by killing it, so back on the topmost screen must do nothing at all.
    BackHandler(enabled = true) {
        when {
            showSearch -> { viewModel.clearSearch(); showSearch = false }
            showSettings -> showSettings = false
            state.currentGroup != null -> viewModel.closeGroup()
            else -> Unit
        }
    }

    Box(Modifier.fillMaxSize()) {
        when {
            showSearch -> RecordSearchScreen(
                state = state,
                onClose = { showSearch = false },
                onLocate = { message ->
                    showSearch = false
                    viewModel.openGroup(local.wechatlite.mobile.data.Group(message.group, message.group))
                },
                onSearch = { viewModel.searchRecords(it) },
                onMore = { viewModel.searchMore() },
                onClear = { viewModel.clearSearch() },
            )

            showSettings -> SettingsScreen(
                state = state,
                onClose = { showSettings = false },
                onDisconnect = { viewModel.disconnect() },
                onReconnect = {
                    state.connection?.let {
                        showSettings = false
                        viewModel.connect(it)
                    }
                },
            )

            state.connection == null -> ConnectScreen(
                connecting = state.connecting,
                error = state.error,
                onConnect = { viewModel.connect(it) },
                onDismissError = { viewModel.dismissError() },
            )

            state.currentGroup == null -> GroupListScreen(
                state = state,
                onOpen = { viewModel.openGroup(it) },
                onRefresh = { viewModel.refresh() },
                onSearch = { showSearch = true },
                onSettings = { showSettings = true },
            )

            else -> ChatScreen(
                state = state,
                onBack = { viewModel.closeGroup() },
                onSend = { viewModel.send() },
                onDraftChange = { viewModel.updateDraft(it) },
                onLoadOlder = { viewModel.loadOlder() },
                onAttach = { attachmentPicker.launch(arrayOf("*/*")) },
                onRemoveAttachment = { viewModel.removeAttachment(it) },
                onPickMention = { viewModel.pickMention(it) },
                onReply = { viewModel.setReplyTo(it) },
                onRecall = { viewModel.recall(it) },
                onOpenAnnouncement = { viewModel.openAnnouncement() },
                onCloseAnnouncement = { viewModel.closeAnnouncement() },
                onOpenRecords = { showSearch = true },
                onTogglePin = { viewModel.togglePinned() },
                onNotificationMode = { viewModel.setNotificationMode(it) },
                onOpenMedia = { message, media ->
                    scope.launch {
                        val file = viewModel.localMedia(message, media)
                        // A failed download reports through `state.error`; only a file that reached
                        // the device but could not be handed off needs its own message here.
                        if (file != null) openFile(context, file)?.let { snackbar.showSnackbar(it) }
                    }
                },
                onPreviewNeeded = { viewModel.requestPreviews(it) },
                onOpenRecord = { viewModel.openRecord(it) },
                onCloseRecord = { viewModel.closeRecord() },
                onSettings = { showSettings = true },
            )
        }
        SnackbarHost(snackbar, modifier = Modifier.padding(16.dp))
    }
}

private fun displayName(context: android.content.Context, uri: Uri): String {
    val resolver = context.contentResolver
    resolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) {
            val index = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            if (index >= 0) cursor.getString(index)?.let { return it }
        }
    }
    return uri.lastPathSegment?.substringAfterLast('/') ?: "附件"
}

private fun sizeOf(context: android.content.Context, uri: Uri): Long {
    val resolver = context.contentResolver
    resolver.query(uri, arrayOf(android.provider.OpenableColumns.SIZE), null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) {
            val index = cursor.getColumnIndex(android.provider.OpenableColumns.SIZE)
            if (index >= 0 && !cursor.isNull(index)) return cursor.getLong(index)
        }
    }
    return 0L
}

/**
 * Hands a downloaded attachment to whichever app the system associates with its type.
 *
 * A wildcard MIME type on `ACTION_VIEW` matches almost nothing, so the previous implementation
 * silently did nothing for documents while images and videos worked. Losing the visible result was
 * the bigger bug: the tap has to either open the file, offer the system share sheet, or say why it
 * cannot. Returns a message to show when nothing could be launched.
 */
private fun openFile(context: android.content.Context, file: File): String? {
    val uri = runCatching {
        FileProvider.getUriForFile(context, "${context.packageName}.files", file)
    }.getOrNull() ?: return "无法共享该附件：${file.name}"
    val mime = mimeOf(file)
    val open = Intent(Intent.ACTION_VIEW).apply {
        setDataAndType(uri, mime)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    // Launch first instead of asking `resolveActivity` up front: from Android 11 package visibility
    // hides other apps from that query, so a null answer does not prove that no viewer exists.
    try {
        context.startActivity(open)
        return null
    } catch (_: android.content.ActivityNotFoundException) {
    } catch (_: SecurityException) {
    }
    // Nothing claims this type. The share sheet always exists and still lets the file leave the app
    // (save it, mail it, hand it to a file manager), which beats a tap that appears to do nothing.
    val share = Intent(Intent.ACTION_SEND).apply {
        type = mime
        putExtra(Intent.EXTRA_STREAM, uri)
        // Without clipData the receiving app is not given the read grant on a stream extra.
        clipData = android.content.ClipData.newRawUri(file.name, uri)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    val chooser = Intent.createChooser(share, "分享 ${file.name}")
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    return try {
        context.startActivity(chooser)
        null
    } catch (_: Exception) {
        // Same wording as the desktop client's `openMedia` failure path.
        "系统未能打开此文件：${file.name}"
    }
}

/**
 * The type used to hand a cached attachment to the system.
 *
 * A wildcard MIME type was the previous answer for every document, and no viewer advertises that
 * wildcard, so documents looked dead while images and videos opened. Ask the platform first, then
 * fall back to a concrete type, because the fallback is what a viewer actually matches.
 */
private fun mimeOf(file: File): String {
    val extension = file.extension.lowercase(Locale.ROOT)
    return android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension)
        ?: local.wechatlite.mobile.ui.fallbackMimeFor(extension)
}
