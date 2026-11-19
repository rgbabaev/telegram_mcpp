# 0006: Sender-filtered history through search

## Context

“Latest message from a person” requires sender identity and retrieval without keywords. The original contract discards senders and requires text queries.

## Decision

Supersede the query/filter contract in ADRs 0003 and 0004. Keep three tools. Permit an omitted query only with a chat ID; explicit blank queries remain invalid. Use typed TDLib sender filters and validate returned senders. Include nullable sender identity and outgoing direction on every message, and TDLib's peer user ID on private-chat listings. Forwarding does not replace the actual sender. Bind cursors to sender type/ID and query.

## Consequences

Existing text searches retain their behavior. Clients must refresh schemas. An initial single-chat sender search with limit one identifies the latest accessible match, including media without captions. Empty partial pages require continuation. Existing bounds, authorization, and Telegram history limitations apply; secret-chat sender filtering returns `history_unavailable`. No additional tools, identity directory, or history mirror are needed.
