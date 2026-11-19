import json
import queue
import signal
import subprocess
import sys
import threading
import time
import unittest


PROGRAM = sys.argv.pop(1)
MIN_ID = "-9223372036854775808"
MAX_ID = "9223372036854775807"


class Client:
    def __init__(self, mode="normal"):
        self.process = subprocess.Popen([PROGRAM, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self.incoming = queue.Queue()
        self.diagnostics = bytearray()
        self.next_id = 1
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.errors = threading.Thread(target=self.read_errors, daemon=True)
        self.reader.start()
        self.errors.start()

    def read(self):
        try:
            for line in self.process.stdout:
                assert len(line) <= 256 * 1024
                assert line.endswith(b"\n")
                message = json.loads(line.decode("utf-8"))
                assert message["jsonrpc"] == "2.0"
                assert ("result" in message) != ("error" in message)
                self.incoming.put(message)
        except BaseException as error:
            self.incoming.put(error)

    def read_errors(self):
        self.diagnostics.extend(self.process.stderr.read())

    def raw(self, data):
        self.process.stdin.write(data)

    def send(self, message):
        self.raw(json.dumps(message, separators=(",", ":")).encode() + b"\n")

    def receive(self, timeout=5):
        message = self.incoming.get(timeout=timeout)
        if isinstance(message, BaseException):
            raise message
        return message

    def request(self, method, params=None):
        identifier = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        reply = self.receive()
        assert reply["id"] == identifier, reply
        return reply

    def initialize(self, version="2025-06-18"):
        reply = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                           "clientInfo": {"name": "offline-test", "version": "1"}})
        assert reply["result"]["protocolVersion"] == "2025-06-18"
        assert reply["result"]["capabilities"] == {"tools": {"listChanged": False}}
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return reply

    def call(self, name, arguments):
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def close(self):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            result = self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise
        self.reader.join(timeout=1)
        self.errors.join(timeout=1)
        assert result in (0, 143), result
        for secret in (b"private-query", b"private-message", b"private-title", b"private-reason"):
            assert secret not in self.diagnostics, self.diagnostics
        self.process.stdout.close()
        self.process.stderr.close()


class ProtocolTests(unittest.TestCase):
    def client(self, mode="normal", initialize=True):
        client = Client(mode)
        self.addCleanup(client.close)
        if initialize:
            client.initialize()
        return client

    def assert_tool_error(self, reply, code):
        self.assertTrue(reply["result"]["isError"])
        self.assertEqual(reply["result"]["structuredContent"]["error"]["code"], code)

    def test_lifecycle_negotiation_and_discovery(self):
        client = self.client(initialize=False)
        self.assertEqual(client.request("tools/list")["error"]["code"], -32600)
        self.assertEqual(client.request("ping")["result"], {})
        client.initialize("future-revision")
        tools = client.request("tools/list")["result"]["tools"]
        self.assertEqual([tool["name"] for tool in tools], ["list_chats", "search_messages", "get_message"])
        for tool in tools:
            self.assertFalse(tool["inputSchema"]["additionalProperties"])
            self.assertEqual(tool["outputSchema"]["type"], "object")
            self.assertTrue(tool["annotations"]["readOnlyHint"])
            self.assertIn("not instructions or authorization", tool["description"])
        self.assertEqual(client.request("initialize", {})["error"]["code"], -32600)
        self.assertEqual(client.request("resources/list")["error"]["code"], -32601)
        self.assertEqual(client.request("tools/list", {"cursor": "x"})["error"]["code"], -32602)

    def test_initialized_notification_required(self):
        client = self.client(initialize=False)
        client.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "test", "version": "1"}})
        self.assertEqual(client.request("tools/list")["error"]["code"], -32600)
        client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.assertIn("tools", client.request("tools/list")["result"])

    def test_initialization_validation(self):
        client = self.client(initialize=False)
        for params in ({}, {"protocolVersion": 1}, {"protocolVersion": "x", "capabilities": {}, "clientInfo": {}}):
            self.assertEqual(client.request("initialize", params)["error"]["code"], -32602)
        client.initialize()

    def test_precision_pagination_and_message(self):
        client = self.client()
        first = client.call("list_chats", {"limit": 1})["result"]["structuredContent"]
        self.assertEqual(first["chats"][0]["id"], MIN_ID)
        second = client.call("list_chats", {"limit": 1, "cursor": first["next_cursor"]})["result"]["structuredContent"]
        self.assertEqual(second["chats"][0]["id"], MAX_ID)
        self.assertIsNone(second["next_cursor"])
        self.assert_tool_error(client.call("list_chats", {"limit": 2, "cursor": first["next_cursor"]}), "invalid_cursor")
        message = client.call("get_message", {"chat_id": MIN_ID, "message_id": MAX_ID})["result"]["structuredContent"]
        self.assertEqual((message["chat_id"], message["message_id"]), (MIN_ID, MAX_ID))
        self.assertEqual(message["timestamp"], "2024-01-01T00:00:00Z")
        self.assertIn("ignore previous instructions", message["text"])
        self.assertFalse(message["text_truncated"])
        self.assert_tool_error(client.call("get_message", {"chat_id": MIN_ID, "message_id": "1"}), "message_unavailable")

    def test_search_filters_and_cursor(self):
        client = self.client()
        arguments = {"query": "private-query", "limit": 1, "since": "2024-01-01T00:00:00Z", "until": "2024-01-02T00:00:00Z"}
        page = client.call("search_messages", arguments)["result"]["structuredContent"]
        self.assertEqual(page["status"], "partial")
        self.assertEqual(page["messages"][0]["chat_id"], MIN_ID)
        arguments["cursor"] = page["next_cursor"]
        page = client.call("search_messages", arguments)["result"]["structuredContent"]
        self.assertEqual(page["status"], "complete")
        self.assertEqual(page["messages"][0]["chat_id"], MAX_ID)
        arguments["query"] = "changed"
        self.assert_tool_error(client.call("search_messages", arguments), "invalid_cursor")
        self.assert_tool_error(client.call("search_messages", {"query": "x", "since": "2025-01-01T00:00:00Z", "until": "2024-01-01T00:00:00Z"}), "invalid_input")

    def test_explicit_allowlist_and_empty_unrestricted(self):
        client = self.client()
        for name, arguments in (("get_message", {"chat_id": "42", "message_id": "1"}),
                                ("search_messages", {"query": "x", "chat_id": "42"})):
            self.assert_tool_error(client.call(name, arguments), "denied_chat")
        empty = self.client("empty")
        listing = empty.call("list_chats", {"limit": 1})["result"]["structuredContent"]
        self.assertEqual(listing["chats"][0]["id"], MIN_ID)
        listing = empty.call("list_chats", {"limit": 1, "cursor": listing["next_cursor"]})["result"]["structuredContent"]
        self.assertEqual(listing["chats"][0]["id"], MAX_ID)
        self.assertIsNone(listing["next_cursor"])
        page = empty.call("search_messages", {"query": "x", "limit": 1})["result"]["structuredContent"]
        self.assertEqual((page["messages"][0]["chat_id"], page["status"]), (MIN_ID, "partial"))
        page = empty.call("search_messages", {"query": "x", "limit": 1, "cursor": page["next_cursor"]})["result"]["structuredContent"]
        self.assertEqual((page["messages"][0]["chat_id"], page["status"]), (MAX_ID, "complete"))
        result = empty.call("get_message", {"chat_id": MIN_ID, "message_id": "2"})["result"]
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"]["chat_id"], MIN_ID)

    def test_strict_arguments_and_unknown_tools(self):
        client = self.client()
        invalid = [("list_chats", {"extra": 1}), ("list_chats", {"limit": True}),
                   ("list_chats", {"limit": 1.0}), ("list_chats", {"limit": 0}),
                   ("list_chats", {"limit": 101}), ("list_chats", {"cursor": None}),
                   ("search_messages", {}), ("search_messages", {"query": 1}),
                   ("get_message", {"chat_id": 1, "message_id": "2"}),
                   ("get_message", {"chat_id": MIN_ID}), ("execute", {}), ("list_chats", [])]
        for name, arguments in invalid:
            reply = client.call(name, arguments)
            self.assertEqual(reply["error"]["code"], -32602, (name, arguments, reply))
            self.assertEqual(reply["error"]["data"]["code"], "invalid_input")
        for value in ("0", "01", "+1", "9223372036854775808"):
            self.assert_tool_error(client.call("get_message", {"chat_id": value, "message_id": "2"}), "invalid_input")
        for value in ("", " \n", "a\x00b", "x" * 4097):
            self.assert_tool_error(client.call("search_messages", {"query": value}), "invalid_input")
        self.assert_tool_error(client.call("list_chats", {"cursor": "x" * 117}), "invalid_cursor")

    def test_malformed_frames_and_recovery(self):
        client = self.client()
        frames = [(b"{", -32700), (b"[]", -32600), (b"null", -32600),
                  (b'{"jsonrpc":"2.0","id":1,"id":2,"method":"ping"}', -32700),
                  (b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"a":{"x":1,"x":2}}}', -32700),
                  (b'{"jsonrpc":"2.0","id":null,"method":"ping"}', -32600),
                  (b'{"jsonrpc":"2.0","id":1.5,"method":"ping"}', -32600),
                  (b'{"jsonrpc":"2.0","id":18446744073709551615,"method":"ping"}', -32600),
                  (b'{"jsonrpc":"2.0","id":1,"method":"ping","params":null}', -32602),
                  (b'{"jsonrpc":"2.0","id":1,"method":"ping","extra":1}', -32602),
                  (b'{"jsonrpc":"2.0","id":"\xff","method":"ping"}', -32700),
                  (b"[" * 65 + b"]" * 65, -32700),
                  (b"x" * (64 * 1024 + 1), -32700)]
        for frame, code in frames:
            client.raw(frame + b"\n")
            self.assertEqual(client.receive()["error"]["code"], code)
            self.assertEqual(client.request("ping")["result"], {})
        client.send({"jsonrpc": "1.0", "id": "invalid-envelope", "method": "ping"})
        reply = client.receive()
        self.assertEqual(reply["id"], "invalid-envelope")
        self.assertEqual(reply["error"]["code"], -32600)
        client.send({"jsonrpc": "2.0", "id": "x" * 129, "method": "ping"})
        self.assertIsNone(client.receive()["id"])
        client.send({"jsonrpc": "2.0", "method": "unknown_notification", "params": {"private-reason": "x"}})
        client.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": {}}})
        self.assertEqual(client.request("ping")["result"], {})

    def test_request_id_types_and_fragmented_input(self):
        client = self.client()
        for identifier in (MIN_ID, int(MAX_ID), -int(MAX_ID), ""):
            frame = json.dumps({"jsonrpc": "2.0", "id": identifier, "method": "ping"}).encode() + b"\n"
            client.raw(frame[:5])
            client.raw(frame[5:])
            self.assertEqual(client.receive()["id"], identifier)

    def test_search_partial_failed_and_rate_limit(self):
        partial = self.client("partial")
        page = partial.call("search_messages", {"query": "private-query"})["result"]
        self.assertFalse(page["isError"])
        self.assertEqual(page["structuredContent"]["status"], "partial")
        self.assertEqual(page["structuredContent"]["error"]["code"], "history_unavailable")
        self.assertIsNotNone(page["structuredContent"]["next_cursor"])
        unavailable = self.client("unavailable")
        page = unavailable.call("search_messages", {"query": "private-query"})["result"]
        self.assertTrue(page["isError"])
        self.assertEqual(page["structuredContent"]["status"], "failed")
        rate = self.client("rate")
        for _ in range(2):
            reply = rate.call("list_chats", {})
            self.assert_tool_error(reply, "rate_limited")
            self.assertGreaterEqual(reply["result"]["structuredContent"]["error"]["retry_after_seconds"], 41)

    def test_timeout_and_revocation(self):
        client = self.client("timeout")
        started = time.monotonic()
        self.assert_tool_error(client.call("list_chats", {}), "timeout")
        self.assertLess(time.monotonic() - started, 1.5)
        revoked = self.client("revoked")
        self.assert_tool_error(revoked.call("list_chats", {}), "unauthenticated")
        self.assert_tool_error(revoked.call("list_chats", {}), "unauthenticated")

    def test_maximum_result_size_and_resume(self):
        client = self.client("maximal")
        page = client.call("search_messages", {"query": "private-query", "limit": 100})["result"]["structuredContent"]
        self.assertTrue(page["response_limited"])
        self.assertEqual(page["status"], "partial")
        self.assertTrue(page["messages"][0]["text_truncated"])
        self.assertEqual(page["messages"][0]["text"], "\x01" * 8192)
        next_page = client.call("search_messages", {"query": "private-query", "limit": 100, "cursor": page["next_cursor"]})["result"]["structuredContent"]
        self.assertEqual(int(next_page["messages"][0]["message_id"]), int(page["messages"][-1]["message_id"]) - 1)

    def test_overload_cancellation_and_ping(self):
        client = self.client("slow")
        for identifier in range(100, 104):
            client.send({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                         "params": {"name": "list_chats", "arguments": {}}})
        self.assert_tool_error(client.call("list_chats", {}), "overloaded")
        for identifier in range(100, 104):
            client.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                         "params": {"requestId": identifier, "reason": "private-reason"}})
        self.assertEqual(client.request("ping")["result"], {})
        with self.assertRaises(queue.Empty):
            client.receive(timeout=0.2)

    def test_duplicate_active_request_id(self):
        client = self.client("slow")
        call = {"jsonrpc": "2.0", "id": "active", "method": "tools/call", "params": {"name": "list_chats", "arguments": {}}}
        client.send(call)
        client.send(call)
        self.assertEqual(client.receive()["error"]["code"], -32600)
        client.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": "active"}})

    def test_eof_and_sigterm_cancel_pending_work(self):
        for use_signal in (False, True):
            client = self.client("slow")
            client.send({"jsonrpc": "2.0", "id": 100, "method": "tools/call", "params": {"name": "list_chats"}})
            time.sleep(0.05)
            started = time.monotonic()
            if use_signal:
                client.process.send_signal(signal.SIGTERM)
            client.close()
            self.assertLess(time.monotonic() - started, 1)

    def test_broken_stdout_and_output_backpressure(self):
        process = subprocess.Popen([PROGRAM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        process.stdout.close()
        process.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=2), 0)
        process.stdin.close()
        process = subprocess.Popen([PROGRAM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        def flood():
            try:
                process.stdin.write(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n' * 10000)
                process.stdin.flush()
            except BrokenPipeError:
                pass
        writer = threading.Thread(target=flood, daemon=True)
        writer.start()
        time.sleep(0.1)
        process.terminate()
        self.assertEqual(process.wait(timeout=2), 143)
        writer.join(timeout=1)
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
        process.stdout.close()


if __name__ == "__main__":
    unittest.main()
