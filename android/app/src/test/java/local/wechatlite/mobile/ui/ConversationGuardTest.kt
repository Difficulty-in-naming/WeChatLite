package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.ChatMessage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Cross-conversation leakage is the worst failure this screen can have: it shows one group's
 * messages inside another group's chat. These cases pin the two guards that prevent it — the row
 * filter and the reply-generation check.
 */
class ConversationGuardTest {

    private fun row(id: String, group: String) = ChatMessage(
        id = id,
        group = group,
        sender = "甲",
        text = "内容",
    )

    @Test
    fun aRowFromAnotherConversationIsRejected() {
        assertFalse(belongsToConversation(row("m1", "b@chatroom"), "a@chatroom"))
    }

    @Test
    fun theOpenConversationAcceptsItsOwnRows() {
        assertTrue(belongsToConversation(row("m1", "a@chatroom"), "a@chatroom"))
    }

    @Test
    fun aRowWithoutAGroupIsAcceptedBecauseHistoricalImportsOmitIt() {
        assertTrue(belongsToConversation(row("m1", ""), "a@chatroom"))
    }

    @Test
    fun filteringKeepsOnlyTheOpenConversationAndUntaggedRows() {
        val page = listOf(
            row("own1", "a@chatroom"),
            row("foreign", "b@chatroom"),
            row("untagged", ""),
            row("own2", "a@chatroom"),
        )
        assertEquals(listOf("own1", "untagged", "own2"), onlyBelonging(page, "a@chatroom").map { it.id })
    }

    @Test
    fun aReplyForTheConversationOnScreenIsApplied() {
        assertTrue(isConversationCurrent("a@chatroom", generation = 3, currentGroup = "a@chatroom", currentGeneration = 3))
    }

    @Test
    fun aReplyForAnEarlierConversationIsDropped() {
        // The user opened A, then B: A's slow reply must not land in B.
        assertFalse(isConversationCurrent("a@chatroom", generation = 3, currentGroup = "b@chatroom", currentGeneration = 4))
    }

    @Test
    fun returningToTheSameConversationStillDropsTheOlderReply() {
        // Left A, came back to A. The id matches again, so only the generation can tell that the
        // reply was requested for the earlier visit and its page is stale.
        assertFalse(isConversationCurrent("a@chatroom", generation = 3, currentGroup = "a@chatroom", currentGeneration = 5))
    }

    @Test
    fun closingTheConversationDropsLateReplies() {
        assertFalse(isConversationCurrent("a@chatroom", generation = 3, currentGroup = null, currentGeneration = 4))
    }
}
