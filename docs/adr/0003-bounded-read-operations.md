# 0003: Bounded allowlisted reads and cursor pagination

## Context

The next MCP adapter needs read operations that cannot widen access, retain result mirrors, or misreport incomplete history searches.

## Decision

Use one `ReadOperations` instance per initialized dispatcher, with an immutable configuration snapshot. Check authorization and chat membership before every typed Telegram read. List configured IDs directly; search each allowed chat separately, in ascending chat-ID order and decreasing message-ID order within each chat. Apply UTC date bounds locally. Bound each search by ten batches, response size, operation concurrency, and one deadline. Report complete/partial/failed explicitly; stop on errors and honor known rate delays without automatic retries.

Use stateless HMAC-SHA256 cursors with a random in-memory instance key, a 15-minute expiry, and operation/filter/page-size/allowlist binding. Encode only position and expiry; retain no result pages. Normalize UTF-8, clip text/title bytes explicitly, and measure serialized JSON within 252 KiB, leaving protocol-envelope space. Generate links only through TDLib. Keep operator discovery separate from allowlisted application reads.

## Consequences

OpenSSL Crypto is required for both build modes; Python validates JSON in tests. Restart invalidates cursors. Pagination is not a snapshot and may yield empty partial pages or require a final empty page to prove exhaustion. Cross-chat ordering favors bounded resumable work over global chronological merging. Narrow date ranges may need multiple calls. Completion reflects Telegram's available search results, not an exhaustive history guarantee. MCP framing and schema validation remain step 4.
