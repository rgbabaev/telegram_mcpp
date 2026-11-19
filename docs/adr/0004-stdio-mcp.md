# 0004: Bounded stdio MCP

## Context

Expose existing read operations through a local client without widening Telegram access or blocking the TDLib dispatcher.

## Decision

Pin MCP 2025-06-18 and verify against the official Python MCP SDK 1.12.4. Vendor nlohmann/json 3.11.3 for offline parsing. Publish exactly three tools with strict input schemas and structured output schemas, including stable error objects. Application operations enforce domain validation and authorization.

Use newline-delimited UTF-8 JSON-RPC, a 64 KiB input-line limit, depth 64, bounded IDs, duplicate-key rejection, at most four asynchronous tool workers without a waiting queue, and eight queued responses. One nonblocking poll loop owns stdio and lifecycle; workers call the read layer. Cancellation suppresses the response and releases workers; already-sent TDLib requests retain dispatcher capacity until completion. EOF, broken pipes, and signals cancel work and close the saved session without logout.

Return data once in `structuredContent`, with a short text notice. Omit the protocol's recommended duplicate serialized text to preserve the 256 KiB response ceiling. The named reference client validates structured content. MCP never prompts for secrets or exposes operator commands.

## Consequences

Stdio-only clients must support structured results. Python dependencies are optional for normal builds and required for reference-client/schema checks. Pin Pydantic 2.11.7 because newer releases remove an internal symbol used by this SDK. Cursors remain process-local; restarting MCP invalidates them. Live saved-session calls and resource measurements require the owner's account; simulated transport checks cannot prove those acceptance criteria.
