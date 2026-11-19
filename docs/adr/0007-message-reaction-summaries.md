# 0007: Message reaction summaries

## Context

Message results omit TDLib reaction summaries, preventing answers about reactions on a message.

## Decision

Expose the same nullable summary in both existing message tools. Preserve reaction types, counts, current-account selection, and tag status. Keep custom emoji IDs as signed decimal strings. Limit summaries to 100 entries and emoji text to 64 UTF-8 bytes, with explicit truncation. Distinguish missing data from an explicitly empty summary.

## Consequences

Clients must refresh output schemas. Existing read authorization and response budgets apply. Summaries use TDLib's latest known state without additional requests, result storage, reactor enumeration, or read receipts. Unknown reaction types remain visible as unsupported.
