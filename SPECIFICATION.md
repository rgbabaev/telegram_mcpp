# Telegram Personal Automation Service — Technical Specification

**Status:** Initial implementation specification for Codex CLI  
**Language:** English  
**Last updated:** 2026-11-08  
**Primary implementation:** C++ with TDLib on Linux  
**Scope:** Personal Telegram account; search, monitoring, and a path to ChatGPT integration

> **Implementation guidance:** Build a small, working vertical slice before adding new features. Distinguish the **confirmed decisions** below from **proposed implementation details**. Do not silently treat optional features as MVP requirements.

## 1. Goal

Build a self-hosted, lightweight Telegram automation service that can authenticate as the owner's **regular Telegram user account** (not merely as a Telegram bot), access the chats available to that account, search message history, monitor incoming messages, and expose approved operations to external clients, with future use from ChatGPT.

The service should run unattended on a Linux machine (local computer, home server, or VPS). Optimize for a maintainable architecture, modest idle RAM/CPU use, and safe handling of Telegram credentials and private messages.

## 2. Decisions established in the discussion

| Area | Decision / conclusion |
|---|---|
| Language | **C++** rather than a Node.js implementation. |
| Telegram SDK | **TDLib** (`tdlib/td`), Telegram's actively maintained C++ client library. |
| Account type | Authenticate as a **normal user**, not through Telegram Bot API. |
| Workload | Initial deployment: **1 account**, approximately **50 monitored chats**, **thousands of messages per day**, and tens of filters/tasks. Larger scale may come later. |
| Initial capabilities | Search message history; list/access chats; monitor new messages; apply filters; retain useful matches. |
| Integration direction | Provide a controlled interface that can eventually be called from **ChatGPT**, for example via an MCP-compatible adapter. An MCP server by itself does **not** guarantee that this ChatGPT session can connect to it; connection and authorization are separate deployment steps. |
| Persistence | **SQLite** is a reasonable lightweight choice for application rules, matches, and task metadata. TDLib manages its own storage/cache separately. |
| Hosting | Native Linux daemon, optionally containerized; `systemd` is the preferred initial deployment method. |
| Performance | C++ avoids Node.js/V8 overhead and JavaScript garbage collection, but TDLib's caches and databases still consume memory. No trustworthy apples-to-apples benchmark was established. |

**Why TDLib:** It handles MTProto, sessions, authorization, network reconnection, asynchronous updates, and local Telegram data. The native C++ integration uses the `Td::TdStatic` CMake target. TDLib also provides a C/JSON API, but use its typed C++ API for this project unless a concrete reason emerges not to.

**Alternatives considered but not chosen:** Node.js + Teleproto/GramJS and Node.js + TDLib; legacy Telegram C/C++ clients (`tgl`, `telegram-cli`); Telegram Bot API (does not provide the required general access to a user account).

## 3. Scope and priorities

### 3.1 MVP — required

1. **Account authentication and session recovery**
   - Accept Telegram `api_id` and `api_hash` from local protected configuration/environment.
   - Support interactive **QR-code login** in a terminal. Support other TDLib-required steps where QR login is not possible, including password/2FA or verification codes when prompted by the current authorization state.
   - Persist the TDLib session in a protected local data directory, restart without requiring login when the session is valid, and report revoked/expired authorization cleanly.
   - Provide `logout` as an explicit operator command; never log out automatically on transient errors.
2. **Chat discovery**
   - List accessible Telegram chats with stable IDs, titles, and basic types.
   - Let the operator select chats by stable ID; avoid relying on mutable titles.
3. **On-demand message search**
   - Search messages across accessible chats and within a specific selected chat.
   - Support a text query and optional date bounds, chat filter, and result limit/pagination where TDLib supports them.
   - Return a structured result: `chat_id`, `chat_title`, `message_id`, timestamp, text/caption if available, and a message link only when one can be validly generated.
   - Do not claim that all history is locally indexed or that all Telegram content is searchable; document Telegram/TDLib limits and handle unavailable history or permissions gracefully.
4. **Continuous monitoring**
   - Subscribe to TDLib's incoming message updates (not polling each chat every few seconds).
   - Allow an operator to define a chat-specific text rule (substring or phrase; case-insensitive by default).
   - Persist matches with their source chat, message identifier, timestamp, matching rule, and a text excerpt.
   - Prevent duplicate stored matches on reconnection, replay, or restart using an appropriate unique key.
5. **Operational CLI**
   - Offer clear commands for login/status, listing chats, searching, managing rules, reading matches, and starting the monitoring daemon.
   - Support human-readable output and machine-readable JSON output for search/matches.
6. **Reliability and observability**
   - Handle temporary network failure, TDLib authorization changes, Telegram rate-limit/flood-wait responses, and graceful termination.
   - Log connection/authorization states, errors, rule counts, processing counts, and useful timings without exposing credentials or private message contents by default.

### 3.2 Next phase — desired, not a blocker for MVP

- A **local-only structured API** for chat discovery, searches, rules, matches, and service health.
- An **MCP adapter** (or equivalent approved integration) exposing a small set of read-only tools to AI clients: `list_chats`, `search_messages`, `list_rules`, `list_matches`, and optionally `create_rule`/`delete_rule` after explicit permission design.
- Scheduling recurring searches and delivery of alerts; preserve results in SQLite. An appropriate notification transport can be selected later.
- Explicitly authorized **send/publish/delete message** operations. These are more sensitive than reads and must be disabled by default and require separate authorization/confirmation.
- Support for several Telegram accounts without sharing sessions or state.

### 3.3 Out of scope for MVP

- A web frontend or mobile application.
- Scraping and downloading every image, video, voice message, or attachment; media recognition/OCR and AI classification.
- Bulk messaging, unsolicited outreach, or automatically deleting messages.
- Hosted third-party cloud services or a Node.js sidecar as a default dependency.
- A production-ready public Internet API before authentication, TLS, and access control have been designed.

## 4. Proposed architecture

Keep modules small, testable, and independently understandable. The suggested shape is:

```text
telegram-automation/
├── CMakeLists.txt
├── README.md
├── config/                 # example configuration, no secrets
├── include/                # public project headers (if useful)
├── src/
│   ├── main.cpp            # CLI entry point
│   ├── telegram/           # TDLib client, authorization, event dispatcher
│   ├── search/             # requests, pagination, result normalization
│   ├── monitoring/         # chat subscriptions, rule matching, deduplication
│   ├── storage/            # app-specific SQLite database, migrations
│   ├── cli/                # command parsing and output
│   └── common/             # config, logging, errors
├── tests/                  # deterministic tests with fake Telegram events
├── deploy/                 # systemd unit and optional container example
└── docs/                   # architecture and operations
```

**Data flow:**

```text
Telegram servers
      |
      v
TDLib (native C++ client; async request/response + updates)
      |
      +----> Search service --------> CLI JSON/human output
      |
      +----> Update dispatcher -----> Rule engine -----> SQLite match store
                                                      |
                                                      v
                                                   CLI/API

Future: ChatGPT <-> authorized MCP adapter <-> restricted local service API
```

**Important implementation details:**

- Use modern **C++17 or newer** with CMake and a pinned, known-good TDLib revision. Do not copy a stale TDLib version number from examples.
- Prefer the official typed C++ TDLib interface, asynchronous request correlation, and a single well-defined event dispatcher.
- Keep blocking SQLite operations and other potentially slow work from stalling TDLib update handling; use bounded queues or an equivalent simple mechanism where needed.
- Do not invent a second full Telegram message cache unless there is a documented functional need. Separate **TDLib-managed data** from the application's SQLite state.
- Avoid unnecessary dependencies or an elaborate dependency-injection framework; standard library + TDLib + SQLite are sufficient initially.
- Treat rules as data: each has a unique ID, chat ID, enabled flag, match mode, query, and creation/update timestamps.
- Have idempotent storage writes and schema migrations. Define retention policy as a configuration setting; choose a reasonable default and document it.

## 5. CLI contract (proposed)

The commands below specify the desired behavior, **not** an already-existing binary interface. Codex may refine their exact spelling while documenting the implemented commands.

```bash
telegram-automation auth qr
telegram-automation auth status
telegram-automation auth logout
telegram-automation chats list --json
telegram-automation search --text "bicycle" --limit 50 --json
telegram-automation search --chat-id 123456789 --text "Trek" --since 2026-11-01 --json
telegram-automation rules add --chat-id 123456789 --contains "React" --ignore-case
telegram-automation rules list --json
telegram-automation rules remove --id 1
telegram-automation matches list --limit 50 --json
telegram-automation serve
telegram-automation status
```

**Output expectations:** JSON responses are parseable, consistently structured, and include error codes/messages without printing secrets. A search result should distinguish an empty result set from an authorization/network error.

## 6. Security and privacy

- Run only with the owner's own authorized Telegram account and only access chats available to that account.
- Protect TDLib's session and encryption material and the application's SQLite database. Use restricted file permissions (`0700` for private directories; restrictive file permissions) and a dedicated Unix user for the daemon where practical.
- Keep `api_hash`, encryption keys, authentication codes, and 2FA passwords out of source control, command logs, error traces, and API responses. Provide `.gitignore` and `.env.example` or an equivalent configuration template.
- Any API should initially **bind to localhost only**. Do not expose it publicly without authenticated access, TLS, request validation, and explicit operator configuration.
- If an MCP adapter is added, expose allowlisted operations and limit returned data; do not expose raw TDLib session files or arbitrary method execution.
- Implement conservative limits for search result size, pagination, stored excerpt length, concurrent requests, and scheduled work.
- Actions that publish/delete messages must not be performed by an autonomous AI tool without explicit user approval and narrow authorization.
- Consider Telegram's terms, privacy expectations of other chat participants, and rate-limit behavior; do not implement rate-limit bypasses.

## 7. Performance requirements and measurement

Performance is a reason for choosing C++, but **do not treat estimates as benchmark results**. The earlier discussion only established qualitative expectations:

- Near-idle CPU while connected and waiting for updates.
- Small per-message processing cost for ordinary text rules.
- Less runtime overhead than a similar Node.js implementation, while TDLib's memory consumption can still be substantial because of caching and local databases.
- Loading a large history can cost much more than monitoring incoming text messages.

**Reference workload:** 1 authenticated account, ~50 chats, several thousand messages/day, tens of active rules/tasks, and bursts of higher arrival rates. A representative 10,000 messages/day equals approximately 0.116 messages/sec on average; peaks, not daily averages, should drive queue limits.

For an initial benchmark, record:

1. Resident and proportional memory (`RSS`, `PSS`) after login and while idle.
2. CPU utilization while idle and during normal message arrivals.
3. Peak RSS/CPU and response time when searching or fetching significant message history.
4. Message-processing latency, match throughput, SQLite write volume, and queue depth during synthetic bursts.
5. Behavior after disconnect/reconnect and process restart (duplicates and missed processing).

Use Linux `pidstat`, `/proc/<pid>/smaps_rollup`, and `/usr/bin/time -v` where appropriate. Report hardware, TDLib revision, test data sizes, and build configuration. Do not set a hard RAM limit without measuring a real build first.

## 8. Testing and acceptance criteria

MVP is considered working when all of the following are demonstrated:

- [ ] Clean CMake build on a documented mainstream Linux distribution.
- [ ] First-time interactive user login works; the running program survives required 2FA steps and can resume with a valid saved session after restart.
- [ ] The program lists the user's available chats and can search history for a phrase in a selected chat.
- [ ] It can search across chats, return bounded results, and emit valid JSON.
- [ ] A rule added for one chat matches a newly received test message in that chat and is ignored for an unrelated chat.
- [ ] Matching events are persisted and survive restarting the process.
- [ ] Replayed/duplicate events do not create duplicate match records.
- [ ] An invalid session, lost connection, flood-wait, and unavailable chat yield meaningful behavior instead of a crash or uncontrolled retry loop.
- [ ] Normal startup, status output, and logs do not disclose secrets or full private messages.
- [ ] The service starts under `systemd`, handles `SIGTERM`, and shuts down cleanly.
- [ ] Unit/integration tests run offline with fixtures or mocked TDLib responses; tests do not require access to the owner's real Telegram account.
- [ ] README documents setup, obtaining Telegram API credentials, TDLib dependency/build, login flow, all implemented commands, known limitations, and test instructions.

## 9. Codex CLI execution plan

Develop incrementally in this order:

**Milestone A — Bootstrap:** Create CMake project, pin/integrate TDLib, write configuration and logging, add an offline smoke test and README build steps.

**Milestone B — Telegram connectivity:** Implement authorization state machine, terminal QR login, persisted session, connection status, and chat listing. Verify against TDLib's current API and examples.

**Milestone C — Search:** Implement per-chat and cross-chat text search with pagination, bounded JSON output, timestamps, and explicit error handling.

**Milestone D — Monitoring:** Implement incoming message dispatcher, configurable chat-specific rules, SQLite schema/migrations, persistence, deduplication, and rule/match CLI commands.

**Milestone E — Operations:** Add daemon mode, `systemd` example, graceful shutdown, metrics/performance test harness, and failure/recovery tests.

**Milestone F — External integration (later):** Design and implement a restricted authenticated local API and then an MCP adapter for permitted read-only operations. Verify actual support and connection requirements in the target ChatGPT product before claiming direct integration works.

For each milestone, provide the source code, tests, exact build/run commands, and a brief record of assumptions or unresolved decisions. Prefer completing and testing one milestone over generating a large unverified codebase.

## 10. Open implementation choices

These were **not conclusively decided** during this conversation. Use the simplest safe implementation initially and document choices instead of blocking progress:

- Exact CLI framework (plain argument parser or lightweight library).
- Exact SQLite library/wrapper and schema, cache/retention policy, and encryption strategy for application data.
- Rule syntax beyond basic case-insensitive substring/phrase matching (regular expressions, full words, hashtags, attachment types).
- Whether message history should ever be mirrored locally for complex offline search.
- Alert transport, scheduling semantics, and whether future outgoing actions are needed.
- MCP transport, client compatibility, authentication model, and permission granularity.
- Whether a local web UI is eventually useful; it is not required for MVP.

## 11. Authoritative references

- TDLib repository, build and CMake integration: https://github.com/tdlib/td
- TDLib getting started (asynchronous API and client lifecycle): https://core.telegram.org/tdlib/getting-started
- Official C++ example (authorization and message updates): https://github.com/tdlib/td/blob/master/example/cpp/td_example.cpp
- TDLib API reference: https://core.telegram.org/tdlib/docs/td__api_8h.html
- TDLib API schema (source of truth for methods/parameters): https://github.com/tdlib/td/blob/master/td/generate/scheme/td_api.tl
- QR authentication method: https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1request_qr_code_authentication.html

---

**Immediate Codex CLI task:** Implement **Milestone A** and then **Milestone B**. Do not begin the MCP adapter, web UI, media processing, or automated sending before a native TDLib-based client can log in, recover its session, and list chats reliably.

