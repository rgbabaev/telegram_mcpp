# 0002: Account session ownership and dispatch

## Context

Operator authentication must preserve sessions, keep secrets out of logs, and support the later MCP process.

## Decision

Lock a permanent `.session.lock` in the configured state root before creating TDLib. TDLib owns `database/` and `files/`; logout cannot remove the lock. Validate ownership, modes, and absence of links before opening existing state. Persist a random 32-byte database key and a successful-authorization marker in private files; never replace a missing key over existing data.

Use one dispatcher thread for typed TDLib send/receive, bounded queued/in-flight requests, correlation, and latest authorization state. Timed-out in-flight requests keep their capacity until their responses arrive. Reserve one control slot for closing. Prompt/render QR codes only on `/dev/tty`; pass QR data to `qrencode` through stdin. Operator commands close TDLib before releasing ownership. Only explicit `auth logout` logs out.

## Consequences

Offline fakes test account operations without TDLib. `qrencode` is a runtime dependency for QR login; `--phone` uses terminal fallback steps. Status reflects locally observed authorization, not proof of connectivity. Backups must preserve the entire private state directory, including its database key. Secret chats and registration/purchase/recovery workflows require an official Telegram app.
