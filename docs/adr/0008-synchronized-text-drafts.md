# 0008: Synchronized text drafts

Supersedes the read-only service boundary in 0003 and the three-tool inventory in 0004. Their access, pagination, and transport decisions remain.

## Context

The owner wants AI-prepared drafts available in Telegram apps, with manual sending.

## Decision

Rename the shared application service to `TelegramOperations`. Add only `set_draft(chat_id, text)`, using typed TDLib `setChatDraftMessage` with plain-text content and no topic/reply. Empty text clears the draft. Reuse read authorization, allowlists, deadlines, concurrency, cancellation, and rate cooldown. Reject secret chats and oversized/invalid text. Mark the MCP tool as a write that replaces existing draft data. No send/schedule API is exposed.

## Consequences

Drafts replace existing content and can be overwritten by other devices. Success means local TDLib acceptance; synchronization is asynchronous. Submitted writes may succeed despite timeout/cancellation, so no automatic retry is performed. Live verification needs an explicitly selected chat and text. Sending and richer draft inputs remain outside scope.
