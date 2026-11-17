# 0001: Bootstrap foundation

## Context

The MVP needs a reproducible typed TDLib dependency and offline verification before account access.

## Decision

Use C++17, CMake/CTest, and TDLib source fetched by immutable revision plus SHA-256. Keep configuration/storage and event logging independent of TDLib. An explicit TDLib-disabled build supports offline tests; production builds enable TDLib by default.

Store flat `key=value` configuration in an owner-only directory. Reject unknown/duplicate keys, unsafe ownership/permissions, and symlinks. Deny access by default through an empty chat allowlist. Log fixed events to an injected stream; the CLI supplies stderr.

## Consequences

The first TDLib build needs network access and its native dependencies. Configuration has no quoting, interpolation, or inline comments. Operator login and MCP remain later steps; configuration checks never contact Telegram.
