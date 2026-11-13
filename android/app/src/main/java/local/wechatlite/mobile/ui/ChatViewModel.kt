package local.wechatlite.mobile.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import java.util.UUID
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import local.wechatlite.mobile.data.Announcement
import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.ConnectionConfig
import local.wechatlite.mobile.data.Group
import local.wechatlite.mobile.data.Media
import local.wechatlite.mobile.data.Member
import local.wechatlite.mobile.data.Repository
import local.wechatlite.mobile.data.ServerEvent
import local.wechatlite.mobile.data.ServerStatus
import local.wechatlite.mobile.data.SettingsStore
import local.wechatlite.mobile.net.ApiClient
import local.wechatlite.mobile.net.EventStream
import local.wechatlite.mobile.net.StreamUpdate
import local.wechatlite.mobile.net.TunnelException
import local.wechatlite.mobile.net.TunnelProcess
import local.wechatlite.mobile.notify.MessageNotifier

/** A pending attachment chosen from the system picker, before it is uploaded. */
data class PendingAttachment(
    val uri: Uri,
    val name: String,
    val size: Long,
    val uploading: Boolean = false,
    val media: Media? = null,
    val error: String? = null,
)

data class ChatUiState(
    val connection: ConnectionConfig? = null,
    val status: ServerStatus? = null,
    val connected: Boolean = false,
    val connecting: Boolean = false,
    val groups: List<Group> = emptyList(),
    val currentGroup: Group? = null,
    val messages: List<ChatMessage> = emptyList(),
    val members: List<Member> = emptyList(),
    val announcement: Announcement? = null,
    val announcementOpen: Boolean = false,
    val recordMessage: ChatMessage? = null,
    val loadingOlder: Boolean = false,
    val hasOlder: Boolean = true,
    val error: String? = null,
    val notice: String? = null,
    val sending: Boolean = false,
    val attachments: List<PendingAttachment> = emptyList(),
    val replyTo: ChatMessage? = null,
    val mentionQuery: String? = null,
    val mentionResults: List<Member> = emptyList(),
    val draft: String = "",
    val notificationMode: String = "all",
    val pinned: Boolean = false,
    val records: List<ChatMessage> = emptyList(),
    val recordsCursor: String? = null,
    val recordsHasMore: Boolean = false,
    val searching: Boolean = false,
    val searchQuery: String = "",
    /** Attachment id → a fully downloaded image or GIF, ready to draw as a thumbnail. */
    val mediaFiles: Map<String, File> = emptyMap(),
    /** Attachment id → the small poster drawn on a video card. */
    val mediaPosters: Map<String, File> = emptyMap(),
    /** Attachment ids whose thumbnail or poster is being fetched right now. */
    val mediaLoading: Set<String> = emptySet(),
    /**
     * How the conversation rows got their summary, for the settings screen: the listing's own
     * `preview`/`timestamp`, the relay's cached newest message, or nothing available. It exists so a
     * screenshot of a blank row can say which of the three happened without device logs.
     */
    val summarySource: String = "",
    /**
     * Milestones of the conversation that was opened last, in milliseconds from the tap:
     * activation, member list, history, first rows, first thumbnail, all previews. It answers
     * "is the chat blank because loading is slow or because downloads are" without device logs.
     */
    val openTiming: String = "",
) {
    val canSend: Boolean get() = status?.canSend == true && currentGroup != null && connected
    val canDownload: Boolean get() = status?.canDownload == true
    val recallSupported: Boolean get() = status?.recallSupported != false
}

class ChatViewModel(application: Application) : AndroidViewModel(application) {
    private val store = SettingsStore(application)
    private val clientId: String = store.clientId()

    private val _state = MutableStateFlow(ChatUiState())
    val state: StateFlow<ChatUiState> = _state.asStateFlow()

    private var api: ApiClient? = null
    private var repo: Repository? = null
    private var eventJob: Job? = null
    private var identity: String = ""
    private val tunnel = TunnelProcess(application)
    private val notifier = MessageNotifier(application)
    private var appInForeground = false

    /** Set when a notification names a group that is not loaded yet; consumed by refreshGroups. */
    var pendingGroupId: String? = null

    // Thumbnails follow the desktop client: two transfers at a time, requested only for rows that
    // are on screen, and remembered failures so a broken attachment is not retried on every frame.
    private val previewSlots = Semaphore(2)
    private val previewing = mutableSetOf<String>()
    private val failedPreviews = mutableSetOf<String>()

    // Ids already probed on disk, so scrolling does not stat the same files on every frame. The scan
    // is repeated once the client gains download permission, because only then may it fetch.
    private val scannedPreviews = mutableSetOf<String>()
    private var scannedWithoutDownload = false
    private var previewGeneration = 0

    // Taps use their own marker: the user asked for the full file even if a thumbnail is running.
    private val downloadRequests = mutableSetOf<String>()

    // Timing for the conversation currently being opened. Reset by `openGroup`, read by the settings
    // screen, so "why is the chat blank for a moment" can be answered without a debugger.
    private var openTrace: OpenTrace? = null

    /**
     * Bumped for every conversation change, mirroring the desktop's `conversationGeneration`.
     *
     * `loadHistory` and the member fetch are suspending calls: the user can leave a slow conversation
     * and open another one while their responses are still in flight. Without this counter the late
     * reply was merged into whatever conversation happened to be on screen, which is how one group
     * ended up displaying another group's messages. Every response is now checked against the
     * generation (and group id) it was requested for, exactly like the desktop callbacks do.
     */
    private var conversationGeneration = 0

    // Live message ids already applied to the list, bounded like the desktop `seenMessages`.
    private val seenMessages = mutableSetOf<String>()
    private val seenMessageOrder = ArrayDeque<String>()

    // Groups whose cached page was already consulted, so a repeated listing does not re-read a
    // history the relay holds nothing in. Cleared on connect and when a live message arrives for
    // that group, because the cache may have gained a row by then.
    private val seededGroups = mutableSetOf<String>()
    // A listing event can arrive while the previous fill is still reading pages; one pass at a time
    // is enough, and the second pass would only repeat requests for the same rows.
    private var fillingSummaries = false

    // Running preview transfers, so a tap on a row whose image is still downloading can wait for
    // that transfer instead of starting a second one for the same file.
    private val previewJobs = mutableMapOf<String, Job>()

    init {
        val saved = store.connectionJson
        if (saved != null) {
            ConnectionConfig.parse(saved)?.let { connect(it) }
        }
    }

    /** Validates and stores an invitation, then opens both the REST and event channels. */
    fun connect(config: ConnectionConfig) {
        disconnect()
        store.connectionJson = invitationJson(config)
        identity = sha256(config.token)
        _state.update { it.copy(connection = config, connecting = true, error = null, groups = emptyList()) }

        viewModelScope.launch {
            // A P2P invitation has no address until the bundled helper reports its loopback
            // listener, so the tunnel must be up before the first request goes out.
            val baseUrl = try {
                when (config) {
                    is ConnectionConfig.Direct -> config.resolveBaseUrl()!!
                    is ConnectionConfig.P2p -> {
                        _state.update { it.copy(notice = "正在建立自动连接…") }
                        tunnel.start(config)
                    }
                }
            } catch (e: Exception) {
                _state.update {
                    it.copy(connecting = false, connected = false, notice = null,
                        error = (e as? TunnelException)?.message ?: "建立自动连接失败")
                }
                return@launch
            }

            val client = ApiClient(config, baseUrl)
            api = client
            repo = Repository(client, File(getApplication<Application>().cacheDir, "wechatlite"))
            // Builds before 0.1.8 could write another conversation's rows into a group's cache. Those
            // files are structurally valid but wrong, so they are dropped once rather than migrated;
            // after this the cache is written through the group filter and stays trustworthy.
            if (!store.messageCachePurged()) {
                repo?.messages?.clearAll()
                store.markMessageCachePurged()
            }

            val status = runCatching { repo!!.status() }.getOrElse {
                _state.update { s ->
                    s.copy(connecting = false, connected = false, notice = null,
                        error = it.message ?: "连接失败")
                }
                return@launch
            }
            _state.update { it.copy(status = status, connecting = false, connected = true, notice = null) }
            refreshGroups()

            eventJob?.cancel()
            eventJob = viewModelScope.launch {
                EventStream(client, store, identity).connect().collect { update ->
                    when (update) {
                        is StreamUpdate.Connected ->
                            _state.update { it.copy(connected = true, connecting = false) }

                        is StreamUpdate.Failed ->
                            _state.update { it.copy(connected = false, error = update.message) }

                        is StreamUpdate.Event -> handleEvent(update.event)
                    }
                }
            }
        }
    }

    fun disconnect() {
        eventJob?.cancel()
        eventJob = null
        api?.close()
        api = null
        // Messages cached from this relay must not be drawn against another one, whose group ids are
        // unrelated; the cache is a head start, never a source of truth across connections.
        repo?.messages?.clearAll()
        repo = null
        tunnel.stop()
        // Anything still in flight belongs to the relay being left; its reply must not be applied to
        // whatever is opened next.
        conversationGeneration++
        previewGeneration++
        previewing.clear()
        failedPreviews.clear()
        scannedPreviews.clear()
        previewJobs.clear()
        scannedWithoutDownload = false
        downloadRequests.clear()
        seenMessages.clear()
        seededGroups.clear()
        seenMessageOrder.clear()
        _state.update { ChatUiState(connection = it.connection) }
        // The desktop clears its unread marks and badge when it connects afresh.
        postBadge()
    }

    fun refresh() {
        viewModelScope.launch {
            val repo = repo ?: return@launch
            runCatching { repo.status() }.onSuccess { status ->
                _state.update { it.copy(status = status, connected = true) }
            }
            refreshGroups()
        }
    }

    private suspend fun refreshGroups() {
        val repo = repo ?: return
        runCatching { repo.groups() }.onSuccess { groups ->
            // A conversation this key can no longer read would keep its unread mark and remembered
            // ids forever, so prune the bookkeeping before merging the fresh list.
            val visible = groups.map { it.id }.toSet()
            seenMessages.retainAll { key -> key.substringBefore('\u0000') in visible }
            seededGroups.retainAll { it in visible }
            _state.update { state ->
                // Unread marks and pins are local-only: the server has no idea whether this device
                // has looked at a conversation yet, so a refresh must not wipe them.
                val previous = state.groups.associateBy { it.id }
                val merged = groups.map { group ->
                    val known = previous[group.id]
                    // A listing that was read just before the newest message was committed must not
                    // roll the row back to the older text; the live preview is newer.
                    val keepLocal = known != null && known.timestamp > group.timestamp
                    group.copy(
                        preview = if (keepLocal) known.preview else group.preview,
                        timestamp = if (keepLocal) known.timestamp else group.timestamp,
                        unread = known?.unread ?: 0,
                        unreadMentions = known?.unreadMentions ?: 0,
                        pinned = store.pinned(group.id),
                    )
                }
                // Drop a selected group the server no longer grants instead of showing stale content.
                val current = state.currentGroup?.let { selected -> merged.firstOrNull { it.id == selected.id } }
                state.copy(groups = sortGroups(merged), currentGroup = current)
            }
            // A notification may name a group that was not loaded when it was tapped.
            pendingGroupId?.let { wanted ->
                pendingGroupId = null
                _state.value.groups.firstOrNull { it.id == wanted }?.let { openGroup(it) }
            }
            // The listing's own state, recorded before any cached text is merged in, so the settings
            // screen reports what the relay sent rather than what the fallback later produced.
            fillMissingSummaries(groups)
        }.onFailure { error ->
            _state.update { it.copy(error = error.message ?: "读取群列表失败") }
        }
    }

    /**
     * Restores the row summaries the listing did not carry.
     *
     * The desktop sidebar always had them: `Window::loadGroups` stores the server's `preview`, and
     * from then on the same window receives every live message. A phone connects later, so if the
     * relay answers `GET /api/groups` without `preview`/`timestamp` the rows would stay blank until
     * the next message arrived. The relay's own cache holds the newest message per group already,
     * so each incomplete row is seeded from it — a plain database read, no WeChat history request.
     *
     * [listed] is the listing before local text was merged in, so a row that only looks complete
     * because an earlier session already filled it is not mistaken for one the relay described.
     */
    private suspend fun fillMissingSummaries(listed: List<Group>) {
        val repo = repo ?: return
        if (_state.value.groups.isEmpty()) return
        if (fillingSummaries) return
        fillingSummaries = true
        try {
            seedMissingSummaries(repo, listed)
        } finally {
            fillingSummaries = false
        }
    }

    private suspend fun seedMissingSummaries(repo: Repository, listed: List<Group>) {
        val missing = listed.filter {
            (normalizePreview(it.preview).isEmpty() || it.timestamp <= 0) && it.id !in seededGroups
        }
        if (missing.isEmpty()) {
            val allListed = listed.all { normalizePreview(it.preview).isNotEmpty() && it.timestamp > 0 }
            _state.update {
                it.copy(summarySource = if (allListed) "群列表自带摘要" else it.summarySource)
            }
            return
        }
        var seeded = 0
        for (group in missing) {
            val page = runCatching { repo.cachedHistory(group.id) }.getOrNull() ?: continue
            // Nothing cached is a definitive answer for now; remembering it keeps a periodic
            // refresh from asking the relay the same question again.
            seededGroups.add(group.id)
            if (page.isEmpty()) continue
            // A live message may have filled the row while the page was being read, so re-check.
            _state.update { state ->
                val index = state.groups.indexOfFirst { it.id == group.id }
                if (index < 0) return@update state
                val current = state.groups[index]
                val filled = seedGroupSummary(current, page)
                if (filled == current) return@update state
                state.copy(groups = sortGroups(state.groups.toMutableList().also { it[index] = filled }))
            }
            val row = _state.value.groups.firstOrNull { it.id == group.id }
            if (row != null && normalizePreview(row.preview).isNotEmpty()) seeded++
        }
        val remaining = _state.value.groups.count { normalizePreview(it.preview).isEmpty() }
        _state.update {
            it.copy(
                summarySource = when {
                    remaining == 0 -> "群列表无摘要，已从本地缓存补齐 $seeded 个会话"
                    seeded == 0 -> "群列表与本地缓存都没有可用的摘要"
                    else -> "群列表无摘要，缓存补齐 $seeded 个会话，仍有 $remaining 个为空"
                },
            )
        }
    }

    private fun sortGroups(groups: List<Group>): List<Group> = sortGroupSummaries(groups)

    fun openGroup(group: Group) {
        forgetPreviews()
        downloadRequests.clear()
        markConversationRead(group.id)
        val generation = ++conversationGeneration
        val trace = OpenTrace(group.id)
        openTrace = trace
        // Draw what the last session stored before any network work starts. This is what makes the
        // open instant: the previous page is already on the device, so the list is never empty for
        // the length of a round trip (or, when the relay has to pull from WeChat, several seconds).
        // Rows are re-checked against the group here because a file written by an earlier build may
        // already contain another conversation's messages; without this they would be restored on
        // every open and the mix-up would look permanent.
        val restored = onlyBelonging(repo?.messages?.read(group.id).orEmpty(), group.id)
        _state.update {
            it.copy(
                currentGroup = group.copy(unread = 0, unreadMentions = 0),
                messages = restored,
                members = emptyList(),
                announcement = null,
                announcementOpen = false,
                recordMessage = null,
                error = null,
                hasOlder = true,
                replyTo = null,
                // A restored list must not show the "还没有消息" placeholder while the real page is
                // still in flight, so it counts as loading until the network answers.
                loadingOlder = restored.isEmpty(),
                notificationMode = store.notificationMode(group.id),
                pinned = store.pinned(group.id),
            )
        }
        if (restored.isNotEmpty()) trace.mark("cache")
        viewModelScope.launch {
            val repo = repo ?: return@launch
            // The desktop's `activateConversation` fires all three requests at once and puts history
            // first. Doing them in sequence added the member list's round trip to the blank screen
            // even though nothing in the first frame needs it. `activate` is still issued first
            // because the server only makes native history readable after it; the request itself is
            // a single POST that runs alongside the history read.
            val activated = launch {
                // Failures here are not fatal: cached pages may still be available.
                runCatching { repo.activate(group.id) }
                trace.mark("activate")
            }
            launch {
                runCatching { repo.members(group.id) }.onSuccess { members ->
                    // The member list belongs to the conversation it was requested for; a reply that
                    // arrives after the user switched away would otherwise land on the new group.
                    _state.update { state ->
                        if (isCurrent(group.id, generation)) state.copy(members = members) else state
                    }
                }
                trace.mark("members")
            }
            loadHistory(older = false, trace = trace, generation = generation)
            trace.mark("history")
            activated.join()
        }
    }

    fun closeGroup() {
        // Drop the stopwatch so a later conversation cannot inherit milestones from this one. The
        // last report stays in the state, which is what the settings screen shows.
        openTrace = null
        // Invalidates in-flight history and member replies for the conversation being left.
        conversationGeneration++
        forgetPreviews()
        _state.update { it.copy(currentGroup = null, messages = emptyList(), members = emptyList()) }
    }

    /**
     * True while the reply being applied still belongs to the conversation on screen.
     *
     * Both conditions matter: the generation detects "left and came back to the same group", where a
     * stale page would otherwise be accepted because the id matches again.
     */
    private fun isCurrent(group: String, generation: Int): Boolean =
        isConversationCurrent(group, generation, _state.value.currentGroup?.id, conversationGeneration)

    /** Opens a conversation by id, used when a notification is tapped. */
    fun openGroupById(groupId: String) {
        val group = _state.value.groups.firstOrNull { it.id == groupId }
        if (group != null) {
            openGroup(group)
        } else {
            pendingGroupId = groupId
            viewModelScope.launch { refreshGroups() }
        }
    }

    /**
     * Clears the unread marker of the conversation the user is looking at, exactly like the desktop
     * client does when its window becomes active. Messages that arrived while the app sat in the
     * background are cleared too; other conversations keep their badges.
     */
    fun markConversationRead(group: String = _state.value.currentGroup?.id.orEmpty()) {
        if (group.isEmpty()) return
        val dirty = _state.value.groups.any { it.id == group && (it.unread > 0 || it.unreadMentions > 0) }
        // Nothing to clear: skip the state write so returning to the app does not redraw every row.
        if (!dirty) return
        _state.update { state ->
            state.copy(
                groups = state.groups.map {
                    if (it.id == group) it.copy(unread = 0, unreadMentions = 0) else it
                },
            )
        }
        postBadge()
    }

    /** Loads one page of older messages; the desktop client retries a `pending` page a few times. */
    fun loadOlder() {
        if (_state.value.loadingOlder || !_state.value.hasOlder) return
        val generation = conversationGeneration
        viewModelScope.launch { loadHistory(older = true, trace = openTrace, generation = generation) }
    }

    private suspend fun loadHistory(older: Boolean, trace: OpenTrace?, generation: Int) {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        if (older) _state.update { it.copy(loadingOlder = true) }
        val before = if (older) _state.value.messages.minOfOrNull { it.seq } ?: 0L else 0L

        var attempt = 0
        while (true) {
            val result = runCatching { repo.history(group.id, before) }
            val page = result.getOrNull()
            // Every await above is a chance for the user to switch conversations. A reply for a
            // conversation that is no longer on screen must be dropped whole — messages, member
            // state, paging flags and the disk write — instead of being merged into the new one.
            if (!isCurrent(group.id, generation)) return
            if (page == null) {
                _state.update {
                    it.copy(loadingOlder = false, error = result.exceptionOrNull()?.message ?: "读取历史失败")
                }
                return
            }
            // The relay answers for the group it was asked about; a page carrying another group's
            // rows means the request and the reply were mixed up, so refuse it rather than draw it.
            val foreign = page.messages.filter { !belongsToConversation(it, group.id) }
            if (foreign.isNotEmpty()) {
                _state.update { it.copy(loadingOlder = false, error = "服务器返回了其他会话的消息，已丢弃") }
                return
            }
            // History is remembered as seen, so the event stream replaying the same rows cannot
            // count them as new unread messages (desktop: `rememberMessage` in `loadHistory`).
            page.messages.forEach { rememberMessage(it) }
            mergeMessages(page.messages, prepend = older)
            // The first page is what ends the blank screen; older pages only extend the list.
            if (!older && page.messages.isNotEmpty()) trace?.mark("rows")
            // Rows that arrived are stored immediately, so the next open draws them from disk even
            // when this page is still `pending` and the retry is going to take a while.
            if (!older && page.messages.isNotEmpty()) persistMessages(group.id)
            if (!page.pending || attempt >= 3) {
                _state.update {
                    it.copy(
                        loadingOlder = false,
                        hasOlder = page.hasMore,
                        openTiming = trace?.report().orEmpty(),
                    )
                }
                trace?.publish()
                return
            }
            // The relay is still pulling from WeChat, so this page is a partial answer. Keep the
            // rows on screen and retry instead of blanking the list; a retry is recorded so the
            // report shows how much of the wait was the relay's pull.
            if (!older) {
                _state.update { it.copy(loadingOlder = false) }
                trace?.mark("wait$attempt")
            }
            attempt++
            kotlinx.coroutines.delay(500L shl attempt)
        }
    }

    /**
     * Writes the open conversation's window to disk.
     *
     * Called from a background thread path (`handleEvent` runs on the event dispatcher), so the file
     * write is off the main thread by construction; failures are swallowed inside [MessageCache]
     * because a dropped cache only costs the next open its head start.
     */
    private fun persistMessages(group: String) {
        val repository = repo ?: return
        val current = _state.value.messages
        if (current.isEmpty() || _state.value.currentGroup?.id != group) return
        // Only this conversation's rows may reach its file. Writing a mixed list would make a
        // one-off mix-up permanent, because the next open would restore it from disk.
        val own = onlyBelonging(current, group)
        if (own.isEmpty()) return
        viewModelScope.launch(Dispatchers.IO) { repository.messages.write(group, own) }
    }

    private fun mergeMessages(incoming: List<ChatMessage>, prepend: Boolean) {
        if (incoming.isEmpty()) return
        _state.update { state ->
            // Last line of defence for cross-conversation leakage: only rows that belong to the
            // conversation on screen may enter its list, whatever path asked for them.
            val current = state.currentGroup?.id
            val accepted = if (current == null) incoming else onlyBelonging(incoming, current)
            if (accepted.isEmpty()) return@update state
            val byId = LinkedHashMap<String, ChatMessage>()
            state.messages.forEach { byId[it.id] = it }
            accepted.forEach { message ->
                // A newer snapshot for the same id wins: it may carry an ack, recall state or media.
                byId[message.id] = merge(message, byId[message.id])
            }
            val ordered = byId.values.sortedWith(compareBy({ it.timestamp }, { it.seq }))
            // The desktop client keeps at most 300 rows in memory, dropping the opposite end of the
            // list from the one being extended: live messages evict the oldest row, while a page of
            // older history evicts the newest. `ordered` is ascending, so a live append must keep
            // the tail and a history prepend must keep the head.
            val bounded = if (ordered.size > MAX_WINDOW) {
                if (prepend) ordered.take(MAX_WINDOW) else ordered.takeLast(MAX_WINDOW)
            } else {
                ordered
            }
            state.copy(messages = bounded)
        }
    }

    private fun merge(fresh: ChatMessage, old: ChatMessage?): ChatMessage {
        if (old == null) return fresh
        // Preserve locally known media paths so a refresh does not force a re-download.
        val media = fresh.media.map { item ->
            val cached = old.media.firstOrNull { it.id == item.id } ?: return@map item
            item.copy(
                localPath = item.localPath.ifEmpty { cached.localPath },
                posterPath = item.posterPath.ifEmpty { cached.posterPath },
            )
        }
        return fresh.copy(media = media)
    }

    private fun handleEvent(event: ServerEvent) {
        when (event) {
            is ServerEvent.Message -> {
                // Remember every frame, replay included: a message that arrives once as history and
                // once as a live event must not be counted as two unread rows.
                val firstSeen = rememberMessage(event.message)
                if (event.message.group == _state.value.currentGroup?.id) {
                    mergeMessages(listOf(event.message), prepend = false)
                    // Keep the file current so reopening the app shows this message immediately,
                    // instead of waiting for the relay's history page.
                    persistMessages(event.message.group)
                }
                if (!event.historical) {
                    refreshGroupPreview(event.message, firstSeen)
                    // A replayed frame of a message that already reached the list must not notify
                    // twice, which is what the desktop's `firstSeen` gate is for.
                    if (firstSeen) maybeNotify(event.message)
                }
            }

            is ServerEvent.Groups -> viewModelScope.launch { refreshGroups() }

            is ServerEvent.Members -> {
                val group = _state.value.currentGroup ?: return
                if (event.group != group.id) return
                viewModelScope.launch {
                    repo?.let { repo ->
                        runCatching { repo.members(group.id) }.onSuccess { members ->
                            _state.update { it.copy(members = members) }
                        }
                    }
                }
            }

            is ServerEvent.Status -> _state.update { state ->
                state.copy(status = event.status, connected = event.status.ready || state.connected)
            }

            is ServerEvent.Warning -> {
                if (event.group.isEmpty() || event.group == _state.value.currentGroup?.id) {
                    _state.update { it.copy(notice = event.text) }
                }
            }

            is ServerEvent.Reset -> {
                forgetPreviews()
                _state.update { it.copy(messages = emptyList(), hasOlder = true) }
            }
            is ServerEvent.Synced -> Unit
            is ServerEvent.Unknown -> Unit
        }
    }

    /**
     * Applies a live message to the conversation list the way `Window::handleEvent` does: the
     * preview and clock follow the newest message, and a message only counts as unread when the
     * user is not actually looking at that conversation.
     */
    private fun refreshGroupPreview(message: ChatMessage, firstSeen: Boolean) {
        // The cache may have gained a row for this group, so a later refresh may consult it again.
        seededGroups.remove(message.group)
        var changed = false
        _state.update { state ->
            val index = state.groups.indexOfFirst { it.id == message.group }
            if (index < 0) return@update state
            val group = state.groups[index]
            var updated = group
            if (message.timestamp >= group.timestamp) {
                updated = updated.copy(preview = previewOf(message), timestamp = message.timestamp)
            }
            if (firstSeen && countsAsUnread(message)) {
                updated = updated.copy(
                    unread = updated.unread + 1,
                    unreadMentions = updated.unreadMentions + if (mentionsSelf(message)) 1 else 0,
                )
            }
            if (updated == group) return@update state
            changed = true
            val groups = state.groups.toMutableList().also { it[index] = updated }
            state.copy(groups = sortGroups(groups))
        }
        if (changed) postBadge()
    }

    /** Same rule as the desktop sidebar: only someone else's delivered message can be unread. */
    private fun countsAsUnread(message: ChatMessage): Boolean {
        if (message.outgoing || message.status != "received") return false
        val looking = message.group == _state.value.currentGroup?.id && appInForeground
        return !looking
    }

    private fun mentionsSelf(message: ChatMessage): Boolean {
        val self = _state.value.status?.selfId.orEmpty()
        return message.mentions.any { it.id == "notify@all" || (self.isNotEmpty() && it.id == self) }
    }

    /**
     * Remembers one message id so a replayed frame cannot be counted twice. Malformed frames are
     * refused instead of being remembered, exactly like the desktop `Window::rememberMessage`.
     */
    private fun rememberMessage(message: ChatMessage): Boolean {
        val id = message.id
        if (id.isBlank() || message.group.isBlank()) return false
        if (id.length > 512 || message.group.length > 512) return false
        val key = message.group + '\u0000' + id
        if (!seenMessages.add(key)) return false
        seenMessageOrder.addLast(key)
        while (seenMessageOrder.size > MAX_SEEN_MESSAGES) {
            seenMessages.remove(seenMessageOrder.removeFirst())
        }
        return true
    }

    /** The launcher badge mirrors the per-group notification level, as on the desktop Dock. */
    private fun unreadTotal(): Int =
        unreadTotalFor(_state.value.groups) { store.notificationMode(it) }

    private fun postBadge() {
        // Runs inside the event collector: a badge failure must never cancel the stream.
        runCatching { notifier.postUnreadBadge(unreadTotal()) }
    }

    private fun previewOf(message: ChatMessage): String = groupPreviewOf(message)

    /**
     * Posts a system notification when the group's notification level calls for one.
     *
     * Historical replay, the account's own messages and duplicate frames must never notify, which
     * is why this runs only for live, non-historical events from someone else.
     */
    private fun maybeNotify(message: ChatMessage) {
        if (message.outgoing || message.status == "recalled") return
        // Notification posting runs on the event-collector coroutine. A malformed message or a
        // platform quirk in `NotificationCompat` must never escape from here: an uncaught throw in
        // this callback cancels the event stream and takes the conversation down with it.
        runCatching {
            notifier.notify(
                message = message,
                selfId = _state.value.status?.selfId.orEmpty(),
                currentGroup = _state.value.currentGroup?.id,
                appActive = _state.value.connected && appInForeground,
            )
        }
    }

    /** The Activity reports its lifecycle so notifications can skip the conversation on screen. */
    fun setForeground(foreground: Boolean) {
        appInForeground = foreground
    }

    fun updateDraft(text: String) {
        _state.update { it.copy(draft = text, mentionQuery = mentionQuery(text)) }
        val query = mentionQuery(text)
        if (query == null) {
            _state.update { it.copy(mentionResults = emptyList()) }
            return
        }
        val members = _state.value.members
        _state.update {
            it.copy(mentionResults = members.filter { member ->
                query.isEmpty() || member.name.contains(query, ignoreCase = true) ||
                    member.remark.contains(query, ignoreCase = true)
            }.take(20))
        }
    }

    /** Replaces the trailing `@query` with a real member mention, as the desktop composer does. */
    fun pickMention(member: Member) {
        _state.update { state ->
            val at = state.draft.lastIndexOf('@')
            val prefix = if (at >= 0) state.draft.substring(0, at) else state.draft
            state.copy(
                draft = "$prefix@${member.name} ",
                mentionQuery = null,
                mentionResults = emptyList(),
                members = state.members,
            )
        }
    }

    fun dismissMentions() {
        _state.update { it.copy(mentionQuery = null, mentionResults = emptyList()) }
    }

    fun addAttachment(uri: Uri, name: String, size: Long) {
        if (size > MAX_FILE) {
            _state.update { it.copy(error = "单个附件不能超过 512 MiB") }
            return
        }
        if (_state.value.attachments.size >= 12) {
            _state.update { it.copy(error = "一次最多附加 12 个文件") }
            return
        }
        _state.update { it.copy(attachments = it.attachments + PendingAttachment(uri, name, size)) }
    }

    fun removeAttachment(uri: Uri) {
        _state.update { s -> s.copy(attachments = s.attachments.filterNot { it.uri == uri }) }
    }

    fun setReplyTo(message: ChatMessage?) {
        _state.update { it.copy(replyTo = message) }
    }

    /**
     * Sends the draft. The message id is generated once and reused across retries so the server can
     * deduplicate, matching the desktop client's stable request id.
     */
    fun send() {
        val repo = repo ?: return
        val state = _state.value
        val group = state.currentGroup ?: return
        if (state.sending) return
        val text = state.draft.trim()
        if (text.isEmpty() && state.attachments.isEmpty()) return

        val sender = store.displayName.ifBlank { "手机端" }
        val messageId = UUID.randomUUID().toString()
        val mentions = resolveMentions(text, state.members)
        val replyId = state.replyTo?.id

        _state.update { it.copy(sending = true, error = null) }
        viewModelScope.launch {
            try {
                val uploaded = state.attachments.map { attachment ->
                    val file = copyToCache(attachment)
                    repo.upload(group.id, file, attachment.name)
                }
                val sent = repo.send(
                    id = messageId,
                    group = group.id,
                    sender = sender,
                    clientId = clientId,
                    text = text,
                    media = uploaded.map { it.id },
                    mentions = mentions,
                    quoteId = replyId,
                )
                mergeMessages(listOf(sent), prepend = false)
                _state.update {
                    it.copy(
                        sending = false,
                        draft = "",
                        attachments = emptyList(),
                        replyTo = null,
                        mentionQuery = null,
                        mentionResults = emptyList(),
                    )
                }
            } catch (e: Exception) {
                _state.update { it.copy(sending = false, error = e.message ?: "发送失败") }
            }
        }
    }

    /** Recalls a confirmed message. Failed and uncertain outcomes are surfaced, never retried. */
    fun recall(message: ChatMessage) {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        viewModelScope.launch {
            runCatching { repo.recall(group.id, message.id) }
                .onSuccess { updated -> mergeMessages(listOf(updated), prepend = false) }
                .onFailure { _state.update { s -> s.copy(error = it.message ?: "撤回失败") } }
        }
    }

    fun openAnnouncement() {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        _state.update { it.copy(announcementOpen = true, announcement = null) }
        viewModelScope.launch {
            runCatching { repo.announcement(group.id) }
                .onSuccess { announcement -> _state.update { it.copy(announcement = announcement) } }
                .onFailure { _state.update { s -> s.copy(error = it.message ?: "读取群公告失败") } }
        }
    }

    fun closeAnnouncement() {
        _state.update { it.copy(announcementOpen = false, announcement = null) }
    }

    fun openRecord(message: ChatMessage) {
        _state.update { it.copy(recordMessage = message) }
    }

    fun closeRecord() {
        _state.update { it.copy(recordMessage = null) }
    }

    fun setNotificationMode(mode: String) {
        val group = _state.value.currentGroup ?: return
        store.setNotificationMode(group.id, mode)
        _state.update { it.copy(notificationMode = mode) }
        // The launcher badge counts a mentions-only group differently, so the level change matters.
        postBadge()
    }

    fun togglePinned() {
        val group = _state.value.currentGroup ?: return
        val next = !store.pinned(group.id)
        store.setPinned(group.id, next)
        _state.update { s ->
            s.copy(
                pinned = next,
                currentGroup = s.currentGroup?.copy(pinned = next),
                groups = sortGroups(s.groups.map { if (it.id == group.id) it.copy(pinned = next) else it }),
            )
        }
    }

    fun dismissError() {
        _state.update { it.copy(error = null) }
    }

    fun dismissNotice() {
        _state.update { it.copy(notice = null) }
    }

    /** Runs one server-side record search page. Results keep the server's newest-first order. */
    fun searchRecords(query: String) {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        _state.update { it.copy(searching = true, searchQuery = query, records = emptyList(), recordsCursor = null) }
        viewModelScope.launch {
            runCatching { repo.records(group.id, query = query) }
                .onSuccess { page ->
                    _state.update {
                        it.copy(
                            searching = false,
                            records = page.messages,
                            recordsCursor = page.nextBefore,
                            recordsHasMore = page.hasMore,
                        )
                    }
                }
                .onFailure { error ->
                    _state.update { it.copy(searching = false, error = error.message ?: "搜索失败") }
                }
        }
    }

    /** Continues the current search with the server's keyset cursor. */
    fun searchMore() {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        val cursor = _state.value.recordsCursor ?: return
        if (_state.value.searching) return
        _state.update { it.copy(searching = true) }
        viewModelScope.launch {
            runCatching { repo.records(group.id, query = _state.value.searchQuery, before = cursor) }
                .onSuccess { page ->
                    _state.update { state ->
                        val known = state.records.map { it.id }.toSet()
                        state.copy(
                            searching = false,
                            records = state.records + page.messages.filterNot { it.id in known },
                            recordsCursor = page.nextBefore,
                            recordsHasMore = page.hasMore,
                        )
                    }
                }
                .onFailure { error ->
                    _state.update { it.copy(searching = false, error = error.message ?: "搜索失败") }
                }
        }
    }

    fun clearSearch() {
        _state.update { it.copy(records = emptyList(), recordsCursor = null, searchQuery = "", recordsHasMore = false) }
    }

    /**
     * Requests previews for the rows that are on screen right now.
     *
     * The desktop client only fetches thumbnails for the viewport, and so does this: [visible]
     * carries the media of those rows, ordered top to bottom. Images and GIFs are downloaded in full
     * only when they are small enough to be worth a thumbnail, videos only get the small server
     * poster, and every other kind stays a file card until the user taps it. Anything a previous
     * session already cached is published without a request.
     */
    fun requestPreviews(visible: List<Media>) {
        val repo = repo ?: return
        val group = _state.value.currentGroup ?: return
        if (visible.isEmpty()) return
        val canDownload = _state.value.canDownload
        if (canDownload && scannedWithoutDownload) {
            scannedPreviews.clear()
            scannedWithoutDownload = false
        }
        if (!canDownload) scannedWithoutDownload = true
        val generation = previewGeneration
        viewModelScope.launch {
            for (media in visible) {
                // The user may have left the conversation while the loop waited for the disk.
                if (generation != previewGeneration || _state.value.currentGroup?.id != group.id) return@launch
                if (media.id.isEmpty()) continue
                val known = if (media.isVideo) _state.value.mediaPosters else _state.value.mediaFiles
                if (known[media.id]?.isFile == true) continue
                if (media.id in scannedPreviews) continue
                val cached = withContext(Dispatchers.IO) { repo.cachedPreview(media, group.id) }
                scannedPreviews.add(media.id)
                if (cached != null) {
                    publishPreview(media, cached)
                    continue
                }
                val fetchable = canDownload &&
                    (media.isVideo || ((media.isImage || media.isGif) && media.size in 1..MAX_THUMBNAIL_BYTES))
                if (fetchable) launchPreview(repo, group.id, media)
            }
        }
    }

    /** Drops preview bookkeeping that belongs to a conversation the user has left. */
    fun forgetPreviews() {
        previewGeneration++
        previewing.clear()
        failedPreviews.clear()
        scannedPreviews.clear()
        previewJobs.clear()
        scannedWithoutDownload = false
        _state.update {
            it.copy(
                mediaFiles = emptyMap(),
                mediaPosters = emptyMap(),
                mediaLoading = emptySet(),
            )
        }
    }

    /** Publishes a finished preview so every row with the same attachment draws it. */
    private fun publishPreview(media: Media, file: File) {
        _state.update {
            if (media.isVideo) {
                it.copy(mediaPosters = it.mediaPosters + (media.id to file))
            } else {
                it.copy(mediaFiles = it.mediaFiles + (media.id to file))
            }
        }
    }

    private fun launchPreview(repo: Repository, group: String, media: Media) {
        val id = media.id
        if (id.isEmpty()) return
        // A remembered failure keeps the labelled card until the user taps it explicitly.
        if (failedPreviews.contains(id) || !previewing.add(id)) return
        _state.update { it.copy(mediaLoading = it.mediaLoading + id) }
        val generation = previewGeneration
        // Lazy start so the job is registered before it can finish; a tap that arrives in between
        // then still waits for this transfer instead of downloading the same file again.
        val job = viewModelScope.launch(start = CoroutineStart.LAZY) {
            val file = try {
                previewSlots.withPermit {
                    if (media.isVideo) repo.poster(media, group) else repo.download(media, group)
                }
            } catch (e: Exception) {
                // A missing thumbnail is not an error the user needs to see: the row keeps its
                // "点击下载" label, exactly like the desktop client's placeholder.
                null
            } finally {
                previewing.remove(id)
                previewJobs.remove(id)
            }
            // Leaving the conversation invalidates the result.
            if (generation != previewGeneration || _state.value.currentGroup?.id != group) return@launch
            if (file == null) {
                if (failedPreviews.size >= MAX_FAILED_PREVIEWS) failedPreviews.clear()
                failedPreviews.add(id)
                // A retry only happens when the user taps the card.
                _state.update { it.copy(mediaLoading = it.mediaLoading - id) }
            } else {
                _state.update {
                    it.copy(
                        mediaLoading = it.mediaLoading - id,
                        messages = it.messages.markMedia(id, group, file, poster = media.isVideo),
                    )
                }
                publishPreview(media, file)
                // First picture is the point the chat stops looking half-loaded; the report is
                // rewritten from then on so both costs stay visible.
                openTrace?.let { trace ->
                    trace.mark("preview")
                    if (previewing.isEmpty()) trace.mark("previewAll")
                    _state.update { state -> state.copy(openTiming = trace.report()) }
                }
            }
        }
        previewJobs[id] = job
        job.start()
    }

    /**
     * Downloads a full attachment for the user and hands back a local file.
     *
     * A thumbnail already on disk is reused, so tapping a picture that is already drawn costs
     * nothing; an image or GIF that is still being fetched as a thumbnail is awaited instead of
     * being downloaded twice.
     */
    suspend fun localMedia(message: ChatMessage, media: Media): File? {
        val repo = repo ?: return null
        val group = _state.value.currentGroup ?: return null
        localFileFor(media)?.let { return it }
        if (!_state.value.canDownload) {
            _state.update { it.copy(error = "当前客户端没有下载权限") }
            return null
        }
        if (!media.isVideo) {
            previewJobs[media.id]?.join()
            localFileFor(media)?.let { return it }
        }
        if (media.id.isNotEmpty() && !downloadRequests.add(media.id)) return null
        failedPreviews.remove(media.id)
        scannedPreviews.remove(media.id)
        // The card shows a spinner while the tap-to-open download runs.
        _state.update { it.copy(mediaLoading = it.mediaLoading + media.id) }
        return try {
            val file = repo.download(media, group.id)
            _state.update { state ->
                state.copy(messages = state.messages.markMedia(media.id, group.id, file, poster = false))
            }
            if (!media.isVideo) publishPreview(media, file)
            file
        } catch (e: Exception) {
            _state.update { it.copy(error = e.message ?: "下载附件失败") }
            null
        } finally {
            downloadRequests.remove(media.id)
            _state.update { it.copy(mediaLoading = it.mediaLoading - media.id) }
        }
    }

    /** The attachment itself, when this device already holds it: a drawn thumbnail or a cached path. */
    private fun localFileFor(media: Media): File? {
        if (!media.isVideo) _state.value.mediaFiles[media.id]?.takeIf { it.isFile }?.let { return it }
        return media.localPath.takeIf { it.isNotEmpty() }?.let(::File)?.takeIf { it.isFile }
    }

    private suspend fun copyToCache(attachment: PendingAttachment): File = withContext(Dispatchers.IO) {
        val context = getApplication<Application>()
        val target = File(context.cacheDir, "uploads/${UUID.randomUUID()}-${attachment.name.safeName()}")
        target.parentFile?.mkdirs()
        context.contentResolver.openInputStream(attachment.uri)?.use { input ->
            target.outputStream().use { output -> input.copyTo(output) }
        } ?: throw IllegalStateException("无法读取所选文件")
        target
    }

    private fun invitationJson(config: ConnectionConfig): String = org.json.JSONObject().apply {
        put("version", 1)
        when (config) {
            is ConnectionConfig.Direct -> {
                put("mode", "direct")
                put("serverUrl", config.serverUrl)
            }

            is ConnectionConfig.P2p -> {
                // Persist the invitation so a restart re-establishes the same tunnel.
                put("mode", "p2p")
                put("serverUrl", "p2p://${config.serverId}")
                put(
                    "p2p",
                    org.json.JSONObject()
                        .put("server_id", config.serverId)
                        .put("relay_url", config.relayUrl)
                        .put("relay_token", config.relayToken)
                        .put("relay_ca", config.relayCa)
                        .put("relay_quic_port", config.relayQuicPort),
                )
            }
        }
        put("token", config.token)
        config.certificatePin?.let { put("certificatePin", it) }
    }.toString()

    private fun sha256(value: String): String =
        java.security.MessageDigest.getInstance("SHA-256")
            .digest(value.toByteArray())
            .joinToString("") { "%02x".format(it) }

    override fun onCleared() {
        // `onCleared` runs on the main thread while the activity is being destroyed, so the
        // teardown must not block or touch sockets there: closing live TLS connections writes to
        // them, and `NetworkOnMainThreadException` inside `onCleared` kills the process. Detach
        // the references synchronously and let the cleanup finish on an IO thread.
        eventJob?.cancel()
        eventJob = null
        val closingApi = api
        api = null
        repo = null
        _state.value = ChatUiState(connection = _state.value.connection)
        CoroutineScope(Dispatchers.IO).launch {
            closingApi?.close()
            runCatching { tunnel.stop() }
        }
        super.onCleared()
    }

    companion object {
        const val MAX_WINDOW = 300
        const val MAX_FILE = 512L * 1024 * 1024

        /** Desktop parity: only attachments up to 8 MiB are downloaded just to draw a thumbnail. */
        const val MAX_THUMBNAIL_BYTES = 8L * 1024 * 1024
        private const val MAX_SEEN_MESSAGES = 4096
        private const val MAX_FAILED_PREVIEWS = 256
    }
}

private fun String.safeName(): String =
    map { if (it.isLetterOrDigit() || it == '.' || it == '-' || it == '_') it else '_' }
        .joinToString("")
        .takeLast(80)
        .ifEmpty { "file" }

/**
 * Records where a preview landed on this device so a later refresh or a tap reuses it: an image or
 * GIF keeps the attachment itself, a video card keeps only the poster.
 */
private fun List<ChatMessage>.markMedia(
    id: String,
    group: String,
    file: File,
    poster: Boolean,
): List<ChatMessage> {
    if (id.isEmpty()) return this
    val path = file.absolutePath
    return map { row ->
        if (row.group != group || row.media.none { it.id == id }) {
            row
        } else {
            row.copy(
                media = row.media.map { item ->
                    if (item.id != id) item
                    else if (poster) item.copy(posterPath = path) else item.copy(localPath = path)
                },
            )
        }
    }
}
