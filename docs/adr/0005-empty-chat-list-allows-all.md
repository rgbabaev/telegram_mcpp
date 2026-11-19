# 0005: Empty chat allowlist permits all chats

## Context

The owner requested that empty `allowed_chat_ids` allow all chats. This supersedes the empty-list denial in ADRs 0001 and 0003.

## Decision

Keep the configuration key required. Empty means unrestricted access to chats accessible through the saved Telegram account; nonempty lists retain strict membership checks. Apply the same policy to all three MCP tools.

For unrestricted listing and cross-chat search, load TDLib's main and archive lists, merge/deduplicate IDs, and retain ascending chat-ID ordering. Bound discovery to ten load requests per operation and the existing deadline. Return a continuation until discovery finishes; never label an incomplete discovery as a complete empty search. Explicit single-chat operations do not require enumeration. Keep cursors bound to the access mode and filters.

## Consequences

Generated empty configuration permits all chats once credentials are supplied. Restrict access by filling the list and restarting. Initial listing/search may require empty continuation pages while TDLib loads chats. Discovery retains no application result mirror; Telegram access/history limits still apply.
