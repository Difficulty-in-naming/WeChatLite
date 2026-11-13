package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.Group

/**
 * Pure helpers for the conversation list.
 *
 * The list row mirrors the desktop sidebar: the newest message's summary on the left, a clock on
 * the right, a red unread pill on the avatar. Keeping the text and ordering rules here makes them
 * testable on the JVM; they never touch the network or the device.
 */
internal const val MAX_PREVIEW_CHARS = 160

/**
 * Characters that occupy no space on screen: whitespace, control codes, the Unicode format
 * characters a chat message can carry (zero-width space, word joiner, BOM) and the Hangul fillers
 * WeChat uses for a visually empty message. A preview made only of these draws as an empty line
 * while `isBlank()` still says it has text, which is exactly how a row ends up with no second line.
 */
private fun isInvisible(character: Char): Boolean = when {
    // `isWhitespace` misses the no-break spaces a pasted message can carry, which also paint as
    // nothing; `isSpaceChar` covers those.
    Character.isWhitespace(character) || Character.isSpaceChar(character) -> true
    character.category == CharCategory.CONTROL || character.category == CharCategory.FORMAT -> true
    character.code == 0x115F || character.code == 0x1160 || character.code == 0x3164 ||
        character.code == 0xFFA0 -> true
    else -> false
}

/** Joiners and variation selectors: invisible alone, but part of the previous glyph's shape. */
private fun isCombining(character: Char): Boolean = when (character.code) {
    0x200C, 0x200D, 0xFE0E, 0xFE0F -> true
    else -> character.category == CharCategory.NON_SPACING_MARK ||
        character.category == CharCategory.ENCLOSING_MARK
}

/** Same wording the server and the system notification use for an attachment preview. */
internal fun mediaKindLabel(kind: String): String = when (kind) {
    "image" -> "图片"
    "gif" -> "GIF"
    "video" -> "视频"
    "voice" -> "语音"
    else -> "文件"
}

/**
 * One-line summary in the same shape the server stores (`sender: body`), so a later group refresh
 * does not visibly rewrite the row. Only the first attachment is named, matching `group_summaries`.
 */
internal fun groupPreviewOf(message: ChatMessage): String {
    // A message whose only text is a filler character has to fall through to the media/attachment
    // wording, otherwise the row would read "小林:" with nothing after the colon.
    val text = normalizePreview(message.text)
    val body = when {
        message.status == "recalled" -> "撤回了一条消息"
        text.isNotEmpty() -> text
        message.media.isNotEmpty() -> message.media.first().let { media ->
            "[${mediaKindLabel(media.kind)}] ${media.name}"
        }
        message.quote != null -> "[引用消息]"
        message.record != null -> "[聊天记录]"
        else -> "[新消息]"
    }
    // The server truncates the finished `sender: body` string, so do the same to keep a live row
    // and a later refresh identical.
    val sender = normalizePreview(message.sender)
    val joined = if (sender.isEmpty()) body else "$sender: $body"
    return normalizePreview(joined).take(MAX_PREVIEW_CHARS)
}

/**
 * Collapses a stored preview to the single visible line the row draws.
 *
 * The server keeps the message text verbatim, so a preview that starts with a line break, or that
 * holds only invisible characters, is a non-empty string that paints as an empty line. Runs of such
 * characters become a single space and the edges are trimmed, which is the same text the desktop's
 * elided single line shows.
 */
internal fun normalizePreview(text: String): String {
    val collapsed = StringBuilder(text.length)
    var separator = false
    for (character in text) {
        // A joiner or an accent belongs to the glyph before it, so it is appended verbatim instead
        // of becoming a space. At the start of the string there is no glyph to join, so it is
        // padding and gets dropped like the rest.
        if (isCombining(character)) {
            if (collapsed.isNotEmpty() && !separator) collapsed.append(character)
            continue
        }
        if (isInvisible(character)) {
            separator = true
            continue
        }
        if (separator && collapsed.isNotEmpty()) collapsed.append(' ')
        separator = false
        collapsed.append(character)
    }
    return collapsed.toString()
}

/**
 * Fills a row whose listing carried no summary from the newest message of the relay's cached page.
 *
 * The desktop list keeps its summaries because the same long-lived window both loads the listing
 * and receives every live message. A phone that connects later starts from the listing alone, so a
 * relay that answers without `preview`/`timestamp` would leave every row blank until the next
 * message arrives. The cached page holds the same data the desktop had after its refresh, so
 * seeding from it restores the desktop's row instead of inventing a new summary.
 *
 * Text already on the row is never replaced: it came from the listing or from a newer live event.
 */
internal fun seedGroupSummary(group: Group, page: List<ChatMessage>): Group {
    if (group.preview.isNotEmpty() && group.timestamp > 0) return group
    val newest = page
        .filter { it.timestamp > 0 }
        .maxWithOrNull(compareBy<ChatMessage> { it.timestamp }.thenBy { it.seq })
        ?: return group
    // A row whose clock is already newer than the cache describes a message the cache lacks, so the
    // cached text would contradict the clock. Leave that row to the live event that produced it.
    if (group.timestamp > newest.timestamp) return group
    val preview = groupPreviewOf(newest)
    return group.copy(
        preview = if (normalizePreview(group.preview).isEmpty()) preview else group.preview,
        timestamp = newest.timestamp,
    )
}

/**
 * Desktop sidebar order: pinned conversations first, then the most recent message on top. The sort
 * is stable, so equal timestamps keep the order the server reported.
 */
internal fun sortGroupSummaries(groups: List<Group>): List<Group> = groups.sortedWith(
    compareByDescending<Group> { it.pinned }.thenByDescending { it.timestamp },
)

/**
 * The launcher badge mirrors what the group's notification level alerts on, exactly like the
 * desktop Dock: an ignored conversation contributes nothing, a mentions-only conversation only its
 * mentioning messages, and the list's own marker stays level-independent.
 */
internal fun unreadTotalFor(groups: List<Group>, mode: (String) -> String): Int =
    groups.sumOf { group ->
        when (mode(group.id)) {
            "ignore" -> 0
            "mentions" -> group.unreadMentions
            else -> group.unread
        }
    }
