package local.wechatlite.mobile.data

import java.io.File
import java.nio.file.Files
import org.json.JSONArray
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The cache is what fills the first screen before the network answers, so a round trip that loses a
 * field is a visible bug: a row would come back without its text, quote, media path or avatar.
 */
class MessageCacheTest {

    private val root: File = Files.createTempDirectory("message-cache").toFile()
    private val cache = MessageCache(root)

    @After
    fun cleanUp() {
        root.deleteRecursively()
    }

    private fun message(
        id: String,
        text: String = "",
        timestamp: Long = 1_000,
        seq: Long = 1,
    ) = ChatMessage(
        id = id,
        group = "g",
        sender = "甲",
        senderId = "m1",
        text = text,
        timestamp = timestamp,
        seq = seq,
    )

    @Test
    fun writeThenReadKeepsEveryFieldTheRowNeeds() {
        val original = message("m", text = "你好").copy(
            avatar = "YWJj",
            status = "sent",
            outgoing = true,
            sourceId = "src",
            quote = Quote(id = "q", sender = "乙", text = "原文"),
            record = ForwardRecord(
                title = "聊天记录",
                items = listOf(ForwardItem(id = "f1", sender = "丙", text = "内容", timestamp = 5, media = listOf(2))),
            ),
            media = listOf(
                Media(
                    id = "a1",
                    name = "报告.pdf",
                    kind = "file",
                    size = 42,
                    sha256 = "abc",
                    localPath = "/tmp/report.pdf",
                    posterPath = "/tmp/poster.jpg",
                ),
            ),
            mentions = listOf(Member(id = "m2", name = "丁")),
        )
        cache.write("g", listOf(original))
        val restored = cache.read("g")

        assertEquals(1, restored.size)
        val row = restored.single()
        assertEquals("你好", row.text)
        assertEquals("m1", row.senderId)
        assertEquals("YWJj", row.avatar)
        assertEquals("sent", row.status)
        assertTrue(row.outgoing)
        assertEquals("src", row.sourceId)
        assertEquals("原文", row.quote?.text)
        assertEquals("聊天记录", row.record?.title)
        assertEquals("内容", row.record?.items?.single()?.text)
        assertEquals(listOf(2), row.record?.items?.single()?.media)
        assertEquals("报告.pdf", row.media.single().name)
        // The local paths are the difference between a thumbnail on the first frame and a download.
        assertEquals("/tmp/report.pdf", row.media.single().localPath)
        assertEquals("/tmp/poster.jpg", row.media.single().posterPath)
        assertEquals("丁", row.mentions.single().name)
    }

    @Test
    fun anUnknownGroupReadsAsEmptyInsteadOfThrowing() {
        assertEquals(emptyList<ChatMessage>(), cache.read("never-written"))
    }

    @Test
    fun onlyTheNewestRowsAreKeptBecauseTheCacheFillsOneScreen() {
        val many = (1..MessageCache.MAX_ROWS + 40).map { message("m$it", text = "第 $it 条") }
        cache.write("g", many)
        val restored = cache.read("g")
        assertEquals(MessageCache.MAX_ROWS, restored.size)
        assertEquals("第 ${MessageCache.MAX_ROWS + 40} 条", restored.last().text)
    }

    @Test
    fun aCorruptFileIsIgnoredRatherThanCrashingTheOpen() {
        val directory = File(root, "messages").apply { mkdirs() }
        File(directory, "g.json").writeText("{not json")
        assertEquals(emptyList<ChatMessage>(), cache.read("g"))
    }

    @Test
    fun writingTwiceReplacesThePreviousWindow() {
        cache.write("g", listOf(message("a", text = "旧")))
        cache.write("g", listOf(message("b", text = "新")))
        assertEquals(listOf("新"), cache.read("g").map { it.text })
    }

    @Test
    fun conversationsDoNotShareAFile() {
        cache.write("g1", listOf(message("a", text = "群一")))
        cache.write("g2", listOf(message("b", text = "群二")))
        assertEquals("群一", cache.read("g1").single().text)
        assertEquals("群二", cache.read("g2").single().text)
    }

    @Test
    fun groupIdsWithPathSeparatorsStayInsideTheCacheDirectory() {
        cache.write("../../etc/passwd", listOf(message("a", text = "x")))
        val readable = cache.read("../../etc/passwd")
        assertEquals("x", readable.single().text)
        // Everything written must still live under the cache root.
        val files = File(root, "messages").listFiles().orEmpty().map { it.name }
        assertTrue(files.isNotEmpty())
        assertFalse(files.any { it.contains('/') })
        assertFalse(File(root.parentFile, "etc/passwd").exists())
    }

    @Test
    fun clearAllRemovesEveryConversation() {
        cache.write("g1", listOf(message("a")))
        cache.write("g2", listOf(message("b")))
        cache.clearAll()
        assertEquals(emptyList<ChatMessage>(), cache.read("g1"))
        assertEquals(emptyList<ChatMessage>(), cache.read("g2"))
    }

    @Test
    fun theSerialisedRowUsesTheRelayFieldNames() {
        val json = ChatMessage.toJson(message("a", text = "行").copy(senderId = "m9"))
        assertEquals("a", json.getString("id"))
        assertEquals("m9", json.getString("sender_id"))
        assertEquals("行", json.getString("text"))
        // A cached array must be parseable as a bare JSON array of message objects.
        val array = JSONArray().put(json)
        assertEquals("行", ChatMessage.from(array.getJSONObject(0)).text)
    }
}
