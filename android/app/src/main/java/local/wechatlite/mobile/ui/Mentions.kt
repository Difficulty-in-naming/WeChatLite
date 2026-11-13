package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.Member

/**
 * Resolves which members a message text actually mentions.
 *
 * The relay validates every mention by checking that the text contains `@<name>`, so a plain
 * substring test would let `@张三丰` also mean "张三" — and the server would then attach a mention
 * the user never intended. Names are matched longest-first and must end at a word boundary.
 */
fun resolveMentions(text: String, members: List<Member>): List<Member> {
    if (members.isEmpty() || !text.contains('@')) return emptyList()

    // Longest names first so "张三丰" wins over "张三" at the same position.
    val byLength = members.sortedByDescending { it.name.length }
    val matched = ArrayList<Member>(members.size)
    var cursor = 0

    while (true) {
        val at = text.indexOf('@', cursor)
        if (at < 0) break
        val rest = text.substring(at + 1)
        val candidate = byLength.firstOrNull { member ->
            member.name.isNotEmpty() &&
                rest.startsWith(member.name) &&
                isBoundary(rest.getOrNull(member.name.length))
        }
        if (candidate != null && matched.none { it.id == candidate.id }) {
            matched.add(candidate)
        }
        cursor = at + 1
    }
    return matched
}

/** End of text or a non-word character counts as a boundary; letters and digits do not. */
private fun isBoundary(next: Char?): Boolean = next == null || !next.isLetterOrDigit()

/** True when the text ends with an unfinished `@query`, which is when the picker should show. */
fun mentionQuery(text: String): String? {
    val at = text.lastIndexOf('@')
    if (at < 0) return null
    // "a@b" is an email-like token, not a mention.
    if (at > 0 && !text[at - 1].isWhitespace()) return null
    val tail = text.substring(at + 1)
    if (tail.any { it.isWhitespace() }) return null
    return tail
}
