package local.wechatlite.mobile.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Recall eligibility, mirroring the desktop client's rules in `canRecallMessage`.
 *
 * A wrong answer here is expensive: the server enforces the same window, so a message that looks
 * recallable locally but is not would only surface as a confusing failure after the tap.
 */
class RecallRulesTest {
    private val now = 1_800_000_000_000L

    private fun message(
        timestamp: Long = now,
        status: String = "sent",
        outgoing: Boolean = true,
        text: String = "你好",
        sourceId: String = "wx:1",
        recallStatus: String = "",
        recallRetryable: Boolean = false,
        media: List<Media> = emptyList(),
        quote: Quote? = null,
    ) = ChatMessage(
        id = "m1",
        group = "g",
        sender = "我",
        text = text,
        media = media,
        quote = quote,
        timestamp = timestamp,
        status = status,
        sourceId = sourceId,
        recallStatus = recallStatus,
        recallRetryable = recallRetryable,
        outgoing = outgoing,
    )

    @Test
    fun recentConfirmedOwnTextIsRecallable() {
        assertTrue(message().canRecall(now))
    }

    @Test
    fun windowIsThreeMinutes() {
        assertTrue(message(timestamp = now - 179_000).canRecall(now))
        assertFalse(message(timestamp = now - 181_000).canRecall(now))
    }

    @Test
    fun incomingMessagesAreNotRecallable() {
        assertFalse(message(outgoing = false).canRecall(now))
    }

    @Test
    fun unconfirmedSendIsNotRecallable() {
        assertFalse(message(status = "queued").canRecall(now))
        assertFalse(message(status = "uncertain").canRecall(now))
    }

    @Test
    fun missingNativeSourceIsNotRecallable() {
        assertFalse(message(sourceId = "").canRecall(now))
    }

    @Test
    fun messagesWithMediaOrQuoteAreNotRecallable() {
        val media = Media(id = "f", name = "a.png", kind = "image", size = 10, sha256 = "0".repeat(64))
        assertFalse(message(media = listOf(media)).canRecall(now))
        assertFalse(message(quote = Quote(id = "q", sender = "他", text = "原话")).canRecall(now))
    }

    @Test
    fun pendingOrUncertainRecallCannotBeResubmitted() {
        assertFalse(message(recallStatus = "pending").canRecall(now))
        assertFalse(message(recallStatus = "uncertain").canRecall(now))
        assertFalse(message(recallStatus = "failed", recallRetryable = false).canRecall(now))
        assertTrue(message(recallStatus = "failed", recallRetryable = true).canRecall(now))
    }

    @Test
    fun recalledMessageIsNotRecallableAgain() {
        assertFalse(message(status = "recalled").canRecall(now))
    }
}

/** Invitation parsing rules that do not require Android's org.json implementation. */
class ConnectionPinTest {
    @Test
    fun normalizesSeparatorsAndCase() {
        val pin = "AB".repeat(32)
        assertEquals(pin.lowercase(), ConnectionConfig.normalizePin("ab:ab:".replace(":", "") + pin.drop(4)))
    }

    @Test
    fun acceptsUppercaseAndColonSeparatedPins() {
        val raw = (0 until 32).joinToString(":") { "AB" }
        assertEquals("ab".repeat(32), ConnectionConfig.normalizePin(raw))
    }

    @Test
    fun rejectsWrongLength() {
        assertNull(ConnectionConfig.normalizePin("ab".repeat(31)))
        assertNull(ConnectionConfig.normalizePin("ab".repeat(33)))
    }

    @Test
    fun rejectsNonHexCharacters() {
        assertNull(ConnectionConfig.normalizePin("zz".repeat(32)))
        assertNull(ConnectionConfig.normalizePin(null))
    }
}
