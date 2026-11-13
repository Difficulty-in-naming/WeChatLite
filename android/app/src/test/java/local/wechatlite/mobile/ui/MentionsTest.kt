package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.Member
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-logic tests for @mentions.
 *
 * These run on the JVM without a device. They exist because the relay rejects a mention whose
 * name is not literally present in the text, so a sloppy match would either drop a real mention or
 * invent one the user never typed.
 */
class MentionsTest {
    private val members = listOf(
        Member(id = "a", name = "张三"),
        Member(id = "b", name = "张三丰"),
        Member(id = "c", name = "李四"),
    )

    @Test
    fun resolvesAnExactName() {
        val matched = resolveMentions("@李四 看一下", members)
        assertEquals(listOf("c"), matched.map { it.id })
    }

    @Test
    fun longerNameWinsOverItsPrefix() {
        // "@张三丰" must not also resolve "张三"; both names share a prefix.
        val matched = resolveMentions("@张三丰 你好", members)
        assertEquals(listOf("b"), matched.map { it.id })
    }

    @Test
    fun prefixWithoutBoundaryDoesNotMatch() {
        // "张三丰" starts with "张三", but the text says a different, longer name that is not a member.
        val matched = resolveMentions("@张三丰丰", members)
        assertTrue("未登记的名字不应产生 mention", matched.isEmpty())
    }

    @Test
    fun resolvesEachNameOnlyOnce() {
        val matched = resolveMentions("@李四 @李四 再来", members)
        assertEquals(1, matched.count { it.id == "c" })
    }

    @Test
    fun ignoresEmailLikeTokens() {
        // "a@b" is not a mention; the picker must not open for it either.
        assertNull(mentionQuery("mail me at a@b"))
        assertTrue(resolveMentions("mail me at a@b", members).isEmpty())
    }

    @Test
    fun reportsUnfinishedQueryForThePicker() {
        assertEquals("张", mentionQuery("@张"))
        assertEquals("", mentionQuery("@"))
        // A space ends the token, so the picker closes.
        assertNull(mentionQuery("@张三 你好"))
        assertNull(mentionQuery("没有艾特"))
    }

    @Test
    fun emptyMemberListNeverMatches() {
        assertTrue(resolveMentions("@张三", emptyList()).isEmpty())
    }

    @Test
    fun textWithoutAtSignNeverMatches() {
        assertTrue(resolveMentions("张三 你好", members).isEmpty())
    }
}
