# telegram_mcpp

Read-only Telegram MCP server under development. **Step 1 (bootstrap)** provides CMake, pinned TDLib, private configuration, stderr event logging, and offline tests. Login, session locking, Telegram operations, and MCP are subsequent steps in [the specification](docs/SPECIFICATION.md).

## Build

Linux; reference environment: Ubuntu 24.04, Clang 18, CMake 3.28. C++17 and CMake 3.22+ are required.

```sh
sudo apt-get update
sudo apt-get install -y clang lld cmake ninja-build gperf libssl-dev zlib1g-dev ca-certificates
CC=clang CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
cmake --build build --target telegram-mcpp bootstrap_tests -j1
ctest --test-dir build --output-on-failure
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

`check` validates configuration and creates/checks the private state directory without contacting Telegram. Paths resolve relative to the configuration directory; absolute state paths are supported. Initialization never overwrites files. Existing directories/files must be owned by the current user with exactly `0700`/`0600`; symlinks and hard-linked configuration files are rejected. Newly created directories/files use those permissions. Use a private directory whose ancestors you trust. `/private/`, `/state/`, and `config.local` are ignored by Git; keep custom paths outside the repository.

Format: one `key=value` per line; surrounding spaces and blank lines are allowed. No quoting, interpolation, or comments. Four required keys: `api_id`, `api_hash`, `state_directory`, `allowed_chat_ids`. Configuration is limited to 64 KiB. Unknown/duplicate keys and malformed values fail without echoing their values.

Chat IDs are comma-separated signed decimal 64-bit integers, with no duplicates or zero. An empty allowlist grants no access. Configured page size is 1–100 (default 20), timeout is 1–30 seconds (default 30), concurrency is 1–4 (default 4). Response/text budgets are fixed at 256 KiB/8 KiB. These settings establish the configuration contract; Telegram request enforcement arrives with read operations.

All bootstrap diagnostics, including help/version, go to stderr; stdout stays empty for future MCP traffic. Logs accept fixed event identifiers, never configuration values or message text. Exit codes: 0 success, 1 operational/configuration error, 2 invalid command. No logs are persisted.

## Offline tests

With the compiler/CMake installed, this path needs neither network, TDLib, nor Telegram credentials:

```sh
CC=clang CXX=clang++ cmake -S . -B build-offline -G Ninja \
  -DTELEGRAM_MCPP_WITH_TDLIB=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-offline
ctest --test-dir build-offline --output-on-failure
```

Tests cover configuration validation/defaults/limits, full-width IDs, empty allowlist, private permissions, symlink/hard-link rejection, special/oversized files, non-overwriting initialization, fixed-event logging, CLI exit codes, and empty stdout. The TDLib-enabled suite also executes a typed synchronous version query without opening a client/session. CI builds both modes on Ubuntu 24.04.
