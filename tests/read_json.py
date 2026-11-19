import json
import subprocess
import sys

result = subprocess.run([sys.argv[1], "--json"], check=True, capture_output=True)
assert not result.stderr
assert len(result.stdout) <= 256 * 1024 - 4096
page = json.loads(result.stdout.decode("utf-8", errors="strict"))
assert page["status"] == "partial"
assert page["response_limited"] is True
assert page["next_cursor"]
for message in page["messages"]:
    assert isinstance(message["chat_id"], str)
    assert isinstance(message["message_id"], str)
    assert len(message["text"].encode("utf-8")) == 8192
    assert message["text_truncated"] is True
    assert message["chat_title_truncated"] is True
    assert message["text"] == "\x01" * 8192
    assert message["timestamp"] == "2024-01-01T00:00:00Z"
    assert len(message["reactions"]["items"]) == 100
    assert message["reactions"]["items"][0]["emoji"] == "\x01" * 64
    assert message["reactions"]["items"][0]["total_count"] == 2147483647
