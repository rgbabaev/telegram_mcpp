# telegram_mcpp

Read-only Telegram MCP server under development. **Steps 1–2** provide CMake, pinned TDLib, private configuration, account login/recovery, exclusive session ownership, operator chat discovery, and offline tests. Read operations and MCP are subsequent steps in [the specification](docs/SPECIFICATION.md).

## Build

Linux; reference environment: Ubuntu 24.04, Clang 18, CMake 3.28. C++17 and CMake 3.22+ are required.

```sh
sudo apt-get update
sudo apt-get install -y clang lld cmake ninja-build gperf libssl-dev zlib1g-dev ca-certificates qrencode
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

Chat IDs are comma-separated signed decimal 64-bit integers, with no duplicates or zero. An empty allowlist grants no access. Configured page size is 1–100 (default 20), timeout is 1–30 seconds (default 30), concurrency is 1–4 (default 4). Response/text budgets are fixed at 256 KiB/8 KiB. These settings establish the configuration contract; Telegram request enforcement arrives with read operations.

All operator output and diagnostics, including help/version, go to stderr; stdout stays empty for future MCP traffic. Logs accept fixed event identifiers, never configuration values or message text. Exit codes: 0 success, 1 operational/configuration error, 2 invalid command. No logs are persisted.

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

Chat discovery deliberately includes chats outside the MCP allowlist so the operator can configure it. Output is ID, type, and quoted title; controls and non-ASCII UTF-8 bytes are hex-escaped to protect the terminal, and clipped titles are marked. Choose main or archive, limit 1–100 (configuration default), and offset 0–9900. Results follow TDLib's current chat-list order; offsets are not snapshots. A stalled load is marked partial and returns exit 1; an exhausted list is explicit. Discovery is bounded by the request timeout. These are operator commands, not the later MCP `list_chats` tool.

## Account smoke test

With your own credentials, run QR login, verify `status`, restart and verify `status` again, then list main/archive chats. For an account with 2FA, verify the password step; test `--phone` when supported by Telegram. Cancel login with Ctrl-C and confirm a subsequent command can acquire the session. Test logout only when you intend to revoke this saved session. Live account authentication cannot be proven with dummy offline credentials.

## Offline tests

With the compiler/CMake installed, this path needs neither network, TDLib, nor Telegram credentials:

```sh
CC=clang CXX=clang++ cmake -S . -B build-offline -G Ninja \
  -DTELEGRAM_MCPP_WITH_TDLIB=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-offline --target check -j1
```

Tests cover configuration validation, full-width IDs, private storage/ownership, restrictive umask, symlink/hard-link rejection including paths containing `..`, authentication and QR refresh, phone/email/code/2FA steps, revocation/recovery, safe failures, bounded correlation, cancellation/timeout/overload, chat discovery, explicit logout, hidden terminal input, and terminal restoration. TDLib-enabled checks also initialize/close/reopen anonymous state and exercise SIGTERM during a real CLI login prompt. Tests use dummy credentials without completing authentication. CI builds both modes on Ubuntu 24.04.
