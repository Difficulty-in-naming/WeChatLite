package local.wechatlite.mobile.ui

import local.wechatlite.mobile.data.ChatMessage

/**
 * Whether a message may enter a conversation's list.
 *
 * A reply can arrive after the user has switched conversations, and the cache file can hold rows
 * written by an older build. Both routes end here, so the rule lives in one place and is tested
 * directly instead of only through the view model.
 *
 * A row with no `group` is accepted on purpose: the relay omits the field for some historical
 * imports, and dropping those would lose legitimate messages. Every row that does name a group must
 * match the open conversation.
 */
fun belongsToConversation(message: ChatMessage, group: String): Boolean =
    message.group.isEmpty() || message.group == group

/** Filters a page or a restored cache down to the rows the open conversation may show. */
fun onlyBelonging(messages: List<ChatMessage>, group: String): List<ChatMessage> =
    messages.filter { belongsToConversation(it, group) }

/**
 * Whether a reply requested for `group` at `generation` may still be applied.
 *
 * Both halves are needed: the generation catches "left and came back to the same conversation",
 * where an older slow page would otherwise be accepted because the id matches again.
 */
fun isConversationCurrent(
    group: String,
    generation: Int,
    currentGroup: String?,
    currentGeneration: Int,
): Boolean = group == currentGroup && generation == currentGeneration
