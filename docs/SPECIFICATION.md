# Telegram MCP Server — Specification

**Platform:** Linux, C++17+, CMake, TDLib
**MVP:** One personal Telegram user account; read-only MCP access

## 1. Goal

Build a self-hosted MCP server that lets an authorized AI client list selected Telegram chats, search history, and retrieve messages using the owner's regular user account.

The MVP MUST work with a named local stdio-capable MCP client. ChatGPT connectivity is deferred until its target product, transport, and authorization requirements are verified.

## 2. Architecture

- Use TDLib's typed C++ API and pin a tested revision.
- Separate Telegram access, application operations, MCP transport, and operator commands.
- One process owns each TDLib data directory. Enforce an exclusive lock; competing processes MUST fail clearly.
- Provide operator commands: `auth login`, `auth status`, `auth logout`, `chats list`, and `mcp serve`. Commands accessing TDLib state MUST respect the lock.
- `mcp serve` owns the session and uses one asynchronous TDLib dispatcher with request correlation and bounded queues.
- MCP calls the application layer directly. No separate HTTP API, message mirror, or application database is required.
- Store credentials, paths, limits, and allowed chat IDs in protected local configuration.

## 3. Authentication and access

- Authenticate a regular user with Telegram `api_id` and `api_hash`.
- Support terminal QR login and TDLib-required fallback steps, including verification codes and 2FA.
- Persist and recover sessions. Report revoked authorization; never automatically log out on transient failures.
- Complete login through operator commands before MCP startup. Never request authentication secrets through MCP.
- Require an explicit chat-ID allowlist; an empty list grants no access. Enforce it on every tool call.
- Operator chat discovery may list accessible chats for configuration. MCP exposes only allowed chats.

## 4. MCP contract

Use stdio. Pin and document a protocol revision supported by the chosen client. Implement its required lifecycle, capabilities, tool discovery, calls, and cancellation. stdout MUST contain only protocol traffic; logs go to stderr.

Expose exactly these tools:

| Tool | Inputs | Result |
|---|---|---|
| `list_chats` | Optional `limit`, `cursor` | Allowed chats: ID, title, type; next cursor |
| `search_messages` | Required nonempty `query`; optional `chat_id`, `since`, `until`, `limit`, `cursor` | Messages; next cursor; completion status |
| `get_message` | Required `chat_id`, `message_id` | One message |

Required semantics:

- Publish strict JSON input and structured output schemas. Reject unknown fields and invalid values.
- Encode chat/message IDs as decimal strings to preserve 64-bit values.
- Use RFC 3339 UTC timestamps. Date bounds are `[since, until)`; reject reversed bounds.
- Default page size: 20; maximum: 100. Default request timeout: 30 seconds; maximum concurrent requests: 4.
- Messages contain chat ID/title, message ID, timestamp, text/caption, content type, and a link only when valid. Unsupported content MUST be identified; do not download media.
- Limit responses to 256 KiB and individual message text to 8 KiB, measured as UTF-8. Indicate truncation explicitly and preserve valid UTF-8/JSON.
- Use opaque cursors bound to the operation and filters. Document ordering, expiry, and invalid-cursor behavior. History changes need not preserve snapshot consistency.
- Cross-chat search MUST search only allowed chats. Filtering a bounded global page afterward is insufficient.
- Distinguish complete, partial, and failed searches. Never report an incomplete search as a complete empty result.
- Define stable errors for invalid input/cursor, unauthenticated session, denied chat, unavailable message/history, rate limit, timeout, and unavailable service. Include known retry guidance; omit secrets and internal traces.
- Treat Telegram content as untrusted data. Tool descriptions MUST state that message text is not instructions or authorization.
- Never expose arbitrary TDLib execution, file access, message mutations, or rule mutations.

## 5. Reliability and privacy

- Handle network loss, authorization changes, cancellation, rate limits, and SIGTERM without crashes or uncontrolled retries. Honor Telegram retry delays.
- Keep slow work off the TDLib dispatcher. Bound queues; reject overload with a clear error.
- Use `0700` private directories and `0600` private files. Exclude credentials and session data from source control.
- Never log authentication secrets. Do not log message contents or search queries by default.
- Log state changes, errors, request timings, and queue counts.
- Do not retain MCP results separately from TDLib-managed data.
- Document Telegram search/history limits; do not promise complete history access or local indexing.

## 6. Deferred scope

These features MUST NOT delay the MCP MVP:

- Remote Streamable HTTP, authentication/TLS, and verified ChatGPT connectivity.
- Monitoring, rules, alerts, scheduling, SQLite match storage, multiple accounts, and systemd deployment.
- Sending/deleting messages, which require separate authorization design.
- Web UI, media downloads, OCR, AI classification, and full-history indexing.

Before implementing monitoring, specify live-only versus downtime catch-up behavior, deduplication, edit/delete handling, and retention. Lossless monitoring requires recovery tests.

## 7. Implementation order

1. **Bootstrap:** CMake, pinned TDLib, protected configuration, logging, offline test harness.
2. **Account access:** Login, session recovery, exclusive ownership, operator chat discovery.
3. **Read operations:** Allowlist enforcement, bounded search, pagination, message retrieval.
4. **MCP:** stdio, schemas, errors, cancellation, integration with a named local client.

Verify each step before expanding scope. Document assumptions and exact build/run commands. A CLI-only implementation is incomplete.

## 8. Acceptance criteria

- Clean build on a documented Linux distribution with pinned TDLib.
- Manual account smoke test proves login, required 2FA, restart recovery, and chat discovery.
- A named local MCP client discovers and successfully calls all three tools using the saved session.
- Offline tests cover schemas, invalid input, ID precision, allowlist enforcement across pagination, output limits, invalid cursors, partial search, and unavailable messages.
- Offline tests cover revoked authorization, rate limits, disconnects, timeouts, cancellation, overload, and exclusive session ownership.
- stdout contains valid MCP traffic only. Logs/errors disclose no secrets or private message contents.
- SIGTERM shuts down cleanly; a subsequent process can reuse the session.
- README covers setup, credentials, build, login, allowlist, client configuration, tools, limits, and tests.
- Record idle/search RSS and CPU with hardware, build settings, and TDLib revision. Make no unmeasured memory or runtime claims.

## 9. References

- TDLib: https://github.com/tdlib/td
- TDLib lifecycle: https://core.telegram.org/tdlib/getting-started
- C++ example: https://github.com/tdlib/td/blob/master/example/cpp/td_example.cpp
- API schema: https://github.com/tdlib/td/blob/master/td/generate/scheme/td_api.tl
- MCP specification: https://modelcontextprotocol.io/specification/

**Immediate task:** Implement steps 1–4 and satisfy the MCP acceptance criteria. Deferred features require a separate task.
