# telegram_mcpp

Read-only Telegram MCP server for one personal account. Implements steps 1–4: protected account access, allowlisted reads, and local stdio MCP. Live account acceptance checks require the owner’s saved session; see [the specification](docs/SPECIFICATION.md).

## Build

Linux; reference environment: Ubuntu 24.04, Clang 18, CMake 3.28. C++17 and CMake 3.22+ are required.

```sh
sudo apt-get update
sudo apt-get install -y clang lld cmake ninja-build gperf libssl-dev zlib1g-dev ca-certificates qrencode python3
CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
cmake --build build --target check -j1
./build/telegram-mcpp --version
```

TDLib's typed C++ API is built from revision [`23383aef218a3123f02b744bfa91bde21056a814`](https://github.com/tdlib/td/tree/23383aef218a3123f02b744bfa91bde21056a814); the archive checksum is enforced in [cmake/TDLib.cmake](cmake/TDLib.cmake). First configuration downloads it. Build only the listed targets to avoid unrelated TDLib binaries. Upstream [build guidance](https://github.com/tdlib/td#building) covers memory-constrained hosts.

For the local single-core, 2 GiB verification build, configuration additionally used `-DTD_ENABLE_PCH=ON -DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"`. No runtime resource claims are made.

## Configuration

```sh
./build/telegram-mcpp config init ./private/config.local
```

Edit the generated file with a local editor. Set `api_id` and `api_hash` from your own [Telegram application](https://my.telegram.org/apps). The generated placeholders deliberately fail validation. Keep credentials out of command arguments and source control.

```text
api_id=12345
api_hash=0123456789abcdef0123456789abcdef
state_directory=state
allowed_chat_ids=
page_size=20
request_timeout_seconds=30
concurrent_requests=4
```

```sh
./build/telegram-mcpp config check ./private/config.local
```

`check` validates configuration and creates/checks the private state directory without contacting Telegram. Paths resolve relative to the configuration directory; absolute state paths are supported. Initialization never overwrites files. Existing directories/files must be owned by the current user with exactly `0700`/`0600`; symlinks and hard-linked configuration files are rejected. Newly created directories/files use those permissions regardless of umask. Path components are checked before traversing `..`. Use a private directory whose ancestors you trust. `/private/`, `/state/`, and `config.local` are ignored by Git; keep custom paths outside the repository.

Format: one `key=value` per line; surrounding spaces and blank lines are allowed. No quoting, interpolation, or comments. Four required keys: `api_id`, `api_hash`, `state_directory`, `allowed_chat_ids`. Configuration is limited to 64 KiB. Unknown/duplicate keys and malformed values fail without echoing their values.

Chat IDs are comma-separated signed decimal 64-bit integers, with no duplicates or zero. An empty `allowed_chat_ids=` allows all accessible chats; a nonempty list restricts access to those IDs. The key remains required. Restart the server after changing it. Configured page size is 1–100 (default 20), timeout is 1–30 seconds (default 30), concurrency is 1–4 (default 4). Response/text budgets are fixed at 256 KiB/8 KiB. Read operations enforce these limits; operator discovery has its separate bounds below.

All operator output and diagnostics, including help/version, go to stderr. Stdout stays empty outside `mcp serve`, which writes protocol traffic only. Logs accept fixed event identifiers, never configuration values or message text. Exit codes: 0 success, 1 operational/configuration error, 2 invalid command. No logs are persisted.

## Account access

```sh
./build/telegram-mcpp auth login ./private/config.local
./build/telegram-mcpp auth login ./private/config.local --phone
./build/telegram-mcpp auth status ./private/config.local
./build/telegram-mcpp chats list ./private/config.local --limit 20
./build/telegram-mcpp chats list ./private/config.local --archive --offset 20 --limit 20
./build/telegram-mcpp auth logout ./private/config.local
```

Login defaults to a terminal QR code scanned from Telegram **Settings > Devices > Link Desktop Device**. QR codes refresh when TDLib supplies a new token. Install `qrencode`, or use `--phone` for phone/code login. TDLib can additionally request email/email-code and 2FA steps. All authentication input is hidden and read from `/dev/tty`; QR tokens are sent to the renderer through stdin and displayed only on that terminal. Ctrl-C cancels login. Registration, premium purchases, password recovery, and other unsupported authorization steps require an official Telegram app.

`status` never prompts. It reports saved authorization observed by TDLib; it does not prove server connectivity. Previously authorized state that becomes unauthorized is reported as revoked. Network errors, timeouts, and cancellation do not trigger an application logout. Known rate-limit delays are reported; the application does not retry authentication automatically. Only `auth logout` requests logout and destroys TDLib session data; it requires connectivity. A submitted logout may finish after interruption; check status afterward.

Each account command locks the configured state root before opening TDLib. A competing process fails immediately. The lock stays until TDLib closes and is never deleted. New TDLib directories/files inherit `0700`/`0600`. Existing state is checked for unsafe modes, owners, links, and special files. SIGINT/SIGTERM cancels operator work and closes TDLib before releasing ownership (exit 130/143). Keep the state root's ancestors trusted.

State layout: `.session.lock`, `.database.key` (random, private database encryption key), `.authorized` (successful-login marker), `database/`, and `files/`. Back up the entire directory with its permissions. Losing the key prevents session recovery; a missing key over existing database data is an error, never an automatic reset. No media is downloaded; secret chats are disabled.

Chat discovery deliberately includes chats outside the MCP allowlist so the operator can configure it. Output is ID, type, and quoted title; controls and non-ASCII UTF-8 bytes are hex-escaped to protect the terminal, and clipped titles are marked. Choose main or archive, limit 1–100 (configuration default), and offset 0–9900. Results follow TDLib's current chat-list order; offsets are not snapshots. A stalled load is marked partial and returns exit 1; an exhausted list is explicit. Discovery is bounded by the request timeout. MCP `list_chats` respects nonempty allowlists and discovers main/archive chats when the list is empty.

## Read operations

The application API is [ReadOperations](include/telegram_mcpp/read_operations.hpp): `list_chats`, `search_messages`, and `get_message`. Initialize the saved session with `AccountAccess::status()` before constructing it. Reuse one instance per dispatcher. The MCP adapter calls this API directly.

Every Telegram read rechecks authorization and the immutable configured allowlist. An empty allowlist permits message retrieval and single-chat search in any accessible chat. Chat listing and cross-chat search discover main and archived chats, deduplicate IDs, and use that scope. Cross-chat search issues only per-chat `searchChatMessages` requests for allowed IDs; it never searches globally. Chat listing follows ascending signed chat ID; search groups by that order and uses decreasing message ID within each chat. Single-chat search has the same message order. Unrestricted discovery attempts at most ten `loadChats` batches of up to 100 chats per call before listing/searching. Until both lists are exhausted, listing returns an empty page with a continuation and search returns an empty `partial` page. Continue with the cursor; loading progress stays in TDLib. Discovery shares the request deadline and retry/cancellation policy. IDs are read transiently from TDLib after loading; no chat/message mirror is retained. No snapshot consistency is promised as history changes.

IDs enter/leave the read API as canonical decimal strings (chat IDs may be negative; message IDs must be positive). Timestamps use RFC 3339 UTC: output is `YYYY-MM-DDTHH:MM:SSZ`; input also accepts up to nine fractional digits and `+00:00`. Bounds are `[since, until)`; reversed bounds, invalid dates, and leap seconds are rejected. Queries must be nonblank valid UTF-8, without NUL, and at most 4096 bytes. Defaults come from configuration; explicit page sizes must be 1–100.

Search examines at most ten TDLib batches of at most 100 messages per call, within one request deadline. Date bounds filter those allowed-chat results; narrow date ranges can require empty continuation pages. `complete` means TDLib exhausted the requested scope; `partial` means a cursor remains, a budget was reached, or an error followed progress; `failed` means an error prevented any search progress. Error results include stable codes and known retry delays. A page can contain no messages and still be partial/failed. Rate limits stop work immediately and suppress subsequent Telegram reads for the known delay (at least one second); there are no automatic retries. Cancellation/revoked authorization terminates the call.

Cursors are opaque, authenticated, bound to the operation, normalized filters, page size, allowlist, and read-service instance. Each expires after 15 minutes; restart, tampering, mismatched inputs, or expiry produces `invalid_cursor`. Repeat the original inputs with `next_cursor`; restart without a cursor when it becomes invalid. Only positions are encoded: no query, credentials, or retained result pages. Cursors may be replayed before expiry.

JSON results keep IDs as strings, escape controls, preserve valid UTF-8, and explicitly mark clipped/repaired text. Message text/captions are capped at 8 KiB and titles at 512 bytes. Serialized results stay below 252 KiB, reserving 4 KiB of the 256 KiB budget for the protocol envelope; `response_limited` and a continuation mark search budget stops. Unknown/service content is `unsupported`. Supported media expose captions/type without downloads; message links come only from TDLib's link method and are omitted when unavailable. Telegram text is untrusted data, never instructions or authorization.

Stable errors: `invalid_input`, `invalid_cursor`, `unauthenticated`, `denied_chat`, `message_unavailable`, `history_unavailable`, `rate_limited`, `timeout`, `cancelled`, `overloaded`, and `unavailable`. Queries, text, titles, and raw Telegram errors are absent from logs. No result cache, mirror, or local index is created. Telegram may restrict searchable history or return fewer matches than exist; `complete` describes TDLib pagination, not proof of exhaustive historical access. Secret-chat search is unsupported.

## MCP server and client

Complete login and configure `allowed_chat_ids` first. Stop other account commands before starting the server:

```sh
./build/telegram-mcpp mcp serve /absolute/path/private/config.local
```

This owns the session lock, restores saved authorization without prompting, and fails on unauthenticated/revoked sessions. MCP [2025-06-18](https://modelcontextprotocol.io/specification/2025-06-18) uses UTF-8 JSON-RPC, one message per newline. Initialization negotiates that revision, advertises only static tools, and requires `notifications/initialized` before tool discovery/calls. `ping` is supported. An unsupported client revision receives the pinned revision; the client must disconnect if incompatible.

A local client’s stdio server entry is:

```json
{
  "mcpServers": {
    "telegram": {
      "command": "/absolute/path/build/telegram-mcpp",
      "args": ["mcp", "serve", "/absolute/path/private/config.local"]
    }
  }
}
```

The verified reference client is the [official Python MCP SDK 1.12.4](https://github.com/modelcontextprotocol/python-sdk/tree/v1.12.4), exercised by `tools/mcp_smoke.py`. It supports structured results and validates the published schemas. Install its pinned compatible dependencies separately from the C++ server (`python3-venv` is required on Ubuntu):

```sh
python3 -m venv .cache/mcp-client
.cache/mcp-client/bin/pip install -r tools/mcp-client-requirements.txt
.cache/mcp-client/bin/python tools/mcp_smoke.py \
  --server "$PWD/build/telegram-mcpp" --config "$PWD/private/config.local" \
  --chat-id=-1001234567890 --message-id=123456789 --query smoke
```

Replace the IDs with an allowed chat and an existing `message_id` returned by `search_messages`; use a harmless search query. This discovers and calls all three tools and prints only pass/fail notices. The server never accepts login secrets through MCP. ChatGPT/remote transport integration remains deferred.

| Tool | Arguments | Structured result |
|---|---|---|
| `list_chats` | Optional `limit`, `cursor` | `chats`, nullable `next_cursor` |
| `search_messages` | Required `query`; optional `chat_id`, `since`, `until`, `limit`, `cursor` | `messages`, nullable `next_cursor`, `status`, `response_limited`, optional `error` |
| `get_message` | Required decimal-string `chat_id`, `message_id` | Message with chat/title, timestamp, text/type, truncation flags, optional link |

Schemas reject unknown argument fields and invalid types; domain validation rejects malformed IDs, dates, cursors, and bounds. Results use `structuredContent` plus a short text notice, avoiding a second serialized copy that would exceed the response budget. Execution failures set `isError` and include `error.code` and known `retry_after_seconds`; failed searches preserve their status/cursor. Partial searches remain successful tool results with explicit status/error. Protocol errors use JSON-RPC codes `-32700` (parse), `-32600` (request/lifecycle), `-32601` (method), `-32602` (arguments; `data.code=invalid_input`), or `-32603` (internal). Raw failures and private content are never echoed in diagnostics.

Transport limits: 64 KiB per input line, nesting depth 64, string request IDs at most 128 UTF-8 bytes or signed 64-bit integer IDs, eight queued responses, and the configured 1–4 active tool calls with no waiting tool queue. Overload returns `overloaded`. Every stdout line, including its newline, is at most 256 KiB. Malformed/oversized lines recover at the next newline; duplicate JSON keys and JSON-RPC batches are rejected. Unknown notifications are ignored. `notifications/cancelled` stops a matching active request without a response; already-sent Telegram work retains dispatcher capacity until its reply. EOF, closed output, Ctrl-C, and SIGTERM cancel work and close TDLib without logout. Session closure may wait up to ten seconds; SIGTERM exits 143.

## Resource measurements

Append `--profile-build 'Ubuntu 24.04; Clang 18; Release -O0 -DNDEBUG; LLD; TD_ENABLE_PCH=ON'` to the smoke command, using your actual settings. It records one second of idle and one search call: elapsed/CPU time, CPU percentage of one core, initial/final/sampled peak RSS, OS, hardware, cgroup limits, and pinned TDLib revision. Sampling is every 20 ms and can miss shorter peaks. It measures the directly launched server process, including its TDLib threads. Save the output with your acceptance results. No live-account runtime figures have been measured in this workspace.

## Account smoke test

With your own credentials, run QR login, verify `status`, restart and verify `status` again, then list main/archive chats. For an account with 2FA, verify the password step; test `--phone` when supported by Telegram. Cancel login with Ctrl-C and confirm a subsequent command can acquire the session. Test logout only when you intend to revoke this saved session. Live account authentication cannot be proven with dummy offline credentials.

## Offline tests

With CMake, a C++17 compiler, OpenSSL development headers, and Python 3 installed, this path needs neither network, TDLib, nor Telegram credentials:

```sh
CC=clang CXX=clang++ cmake -S . -B build-offline -G Ninja \
  -DTELEGRAM_MCPP_WITH_TDLIB=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-offline --target check -j1
```

Tests cover configuration validation, full-width IDs, private storage/ownership, restrictive umask, symlink/hard-link rejection including paths containing `..`, authentication and QR refresh, phone/email/code/2FA steps, revocation/recovery, safe failures, bounded correlation, cancellation/timeout/overload, chat discovery, explicit logout, hidden terminal input, and terminal restoration. Read tests cover allowlist enforcement on every page, full-width ID strings, date bounds, opaque cursors, bounded/partial/failed search, UTF-8/JSON byte limits, unavailable messages, valid links, retry cooldown, cancellation, overload, and operation deadlines. Python independently parses worst-case serialized output. TDLib-enabled checks verify typed read mappings and initialize/close/reopen anonymous state and exercise SIGTERM during a real CLI login prompt. Tests use dummy credentials without completing authentication. CI builds both modes on Ubuntu 24.04.

To include the independent JSON Schema and official SDK checks (no Telegram credentials):

```sh
cmake -S . -B build-offline \
  -DTELEGRAM_MCPP_MCP_CLIENT_PYTHON="$PWD/.cache/mcp-client/bin/python"
cmake --build build-offline --target check -j1
```

MCP tests exercise lifecycle, discovery, strict arguments, full-width IDs, pagination, structured output schemas, partial/failed searches, output budgets, cancellation, revocation, rate delays, timeouts, overload, broken pipes, backpressure, and SIGTERM with a simulated Telegram transport. TDLib-enabled lifecycle checks also verify that `mcp serve` respects ownership and refuses anonymous sessions without stdout traffic. The named client passes all three calls against the simulated transport; successful saved-account calls, login/2FA, and real idle/search resource figures remain owner-run acceptance checks.
