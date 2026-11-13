package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.ChatMessage
import local.wechatlite.mobile.data.Group
import local.wechatlite.mobile.data.Media
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-logic tests for the conversation list row.
 *
 * These run on the JVM without a device or a server. They pin the two rules that are easy to break
 * silently: the preview text must be shaped exactly like the server's `group_summaries` (otherwise
 * every refresh visibly rewrites the row), and the unread pill must follow the desktop sidebar
 * rather than WeChat's member count.
 */
class GroupSummaryTest {
    private fun message(
        text: String = "",
        sender: String = "小林",
        status: String = "received",
        media: List<Media> = emptyList(),
        timestamp: Long = 0,
        seq: Long = 0,
    ) = ChatMessage(
        id = "m1",
        group = "g",
        sender = sender,
        text = text,
        status = status,
        media = media,
        timestamp = timestamp,
        seq = seq,
    )

    @Test
    fun prefixesTheSenderLikeTheServerDoes() {
        assertEquals("小林: 晚点到", groupPreviewOf(message(text = "晚点到")))
    }

    @Test
    fun omittedSenderFallsBackToTheBareBody() {
        assertEquals("晚点到", groupPreviewOf(message(text = "晚点到", sender = "")))
    }

    @Test
    fun namesTheFirstAttachmentWhenThereIsNoText() {
        val media = Media(id = "a", name = "开场.mov", kind = "video", size = 1024, sha256 = "")
        assertEquals("小林: [视频] 开场.mov", groupPreviewOf(message(media = listOf(media))))
    }

    @Test
    fun summarisesARecallInsteadOfItsText() {
        assertEquals("小林: 撤回了一条消息", groupPreviewOf(message(text = "悄悄话", status = "recalled")))
    }

    @Test
    fun collapsesLineBreaksIntoOneRow() {
        assertEquals("小林: 第一行 第二行", groupPreviewOf(message(text = "第一行\n第二行")))
    }

    @Test
    fun truncatesTheFinishedPreview() {
        val preview = groupPreviewOf(message(text = "x".repeat(400)))
        assertEquals(MAX_PREVIEW_CHARS, preview.length)
        assertTrue(preview.startsWith("小林: xxx"))
    }

    @Test
    fun aPreviewOfInvisibleCharactersNormalisesToEmpty() {
        // `isBlank` is false for a zero-width space and a Hangul filler, so the row drew a blank
        // second line with no fallback text. Normalisation has to empty it for the fallback to run.
        assertEquals("", normalizePreview("\u200b\u3164"))
        assertEquals("", normalizePreview("  \n\t "))
        assertEquals("", normalizePreview("\u200d"))
        assertEquals("小林: 到了", normalizePreview("\u200b小林: 到了\u200b"))
    }

    @Test
    fun joinersStayInsideTheirEmoji() {
        // A zero-width joiner decides how its neighbours render; dropping it would split a family
        // emoji into separate people.
        val family = "\uD83D\uDC68\u200D\uD83D\uDC69\u200D\uD83D\uDC67"
        assertEquals(family, normalizePreview(family))
        assertEquals("全家 $family", normalizePreview("全家\u200b$family"))
    }

    @Test
    fun pinnedConversationsComeFirstThenNewestMessage() {
        val groups = listOf(
            Group(id = "old", name = "旧的", timestamp = 100),
            Group(id = "new", name = "新的", timestamp = 300),
            Group(id = "pin", name = "置顶的", timestamp = 50, pinned = true),
        )
        assertEquals(listOf("pin", "new", "old"), sortGroupSummaries(groups).map { it.id })
    }

    @Test
    fun equalTimestampsKeepTheServerOrder() {
        val groups = listOf(
            Group(id = "a", name = "A", timestamp = 100),
            Group(id = "b", name = "B", timestamp = 100),
        )
        assertEquals(listOf("a", "b"), sortGroupSummaries(groups).map { it.id })
    }

    @Test
    fun badgeFollowsTheNotificationLevel() {
        val groups = listOf(
            Group(id = "all", name = "全部", unread = 3, unreadMentions = 1),
            Group(id = "mentions", name = "仅@我", unread = 5, unreadMentions = 2),
            Group(id = "ignore", name = "免打扰", unread = 9, unreadMentions = 4),
        )
        val modes = mapOf("all" to "all", "mentions" to "mentions", "ignore" to "ignore")
        assertEquals(3 + 2 + 0, unreadTotalFor(groups) { modes.getValue(it) })
    }

    @Test
    fun unreadIsIndependentOfTheMemberCount() {
        // `count` is WeChat's member count; showing it as "new messages" was the old bug.
        val group = Group(id = "g", name = "研发", count = 42, unread = 1)
        assertEquals(42, group.count)
        assertEquals(1, unreadTotalFor(listOf(group)) { "all" })
    }

    @Test
    fun aRowWithoutASummaryIsSeededFromTheNewestCachedMessage() {
        // The listing carried neither `preview` nor `timestamp`, so the row would stay blank.
        val row = Group(id = "g", name = "研发")
        val page = listOf(
            message(text = "早上好", timestamp = 1_000, seq = 3),
            message(text = "晚点到", timestamp = 2_000, seq = 4),
            message(text = "还没定", timestamp = 1_500, seq = 5),
        )
        val seeded = seedGroupSummary(row, page)
        assertEquals("小林: 晚点到", seeded.preview)
        assertEquals(2_000, seeded.timestamp)
    }

    @Test
    fun aRowWithASummaryIsLeftAlone() {
        // Live events are newer than the cached page, so a refresh must not roll the row back.
        val row = Group(id = "g", name = "研发", preview = "小林: 刚刚", timestamp = 9_000)
        val seeded = seedGroupSummary(row, listOf(message(text = "旧消息", timestamp = 1_000)))
        assertEquals("小林: 刚刚", seeded.preview)
        assertEquals(9_000, seeded.timestamp)
    }

    @Test
    fun aMissingClockIsFilledWithoutRewritingThePreview() {
        val row = Group(id = "g", name = "研发", preview = "小林: 刚刚")
        val seeded = seedGroupSummary(row, listOf(message(text = "旧消息", timestamp = 1_000)))
        assertEquals("小林: 刚刚", seeded.preview)
        assertEquals(1_000, seeded.timestamp)
    }

    @Test
    fun anEmptyCacheLeavesTheRowUntouched() {
        val row = Group(id = "g", name = "研发")
        assertEquals(row, seedGroupSummary(row, emptyList()))
    }

    @Test
    fun cachedMessagesWithoutTimestampsAreIgnored() {
        // A row's clock and its text must always describe the same message.
        val row = Group(id = "g", name = "研发")
        assertEquals(row, seedGroupSummary(row, listOf(message(text = "无时间"))))
    }
}
