from jsonschema import Draft202012Validator, FormatChecker
from mcp_stdio import Client, MIN_ID, MAX_ID


def validate_schemas():
    for mode in ("normal", "empty", "partial", "rate", "unavailable", "maximal", "revoked", "latest", "unknown-sender",
                 "reactions", "empty-reactions", "truncated-reactions"):
        client = Client(mode)
        try:
            client.initialize()
            tools = {tool["name"]: tool for tool in client.request("tools/list")["result"]["tools"]}
            calls = [("list_chats", {}), ("search_messages", {"query": "private-query", "limit": 100}),
                     ("search_messages", {"chat_id": MIN_ID, "limit": 1}),
                     ("search_messages", {"chat_id": MIN_ID, "sender": {"type": "user", "id": MAX_ID}, "limit": 1}),
                     ("get_message", {"chat_id": MIN_ID, "message_id": MAX_ID})]
            for name, arguments in calls:
                tool = tools[name]
                for key in ("inputSchema", "outputSchema"):
                    Draft202012Validator.check_schema(tool[key])
                Draft202012Validator(tool["inputSchema"], format_checker=FormatChecker()).validate(arguments)
                result = client.call(name, arguments)["result"]
                schema = Draft202012Validator(tool["outputSchema"], format_checker=FormatChecker())
                schema.validate(result["structuredContent"])
                extended = dict(result["structuredContent"], unexpected=True)
                assert not schema.is_valid(extended)
            if mode == "normal":
                for name, arguments in (("get_message", {"chat_id": MIN_ID, "message_id": "1"}),
                                        ("get_message", {"chat_id": "42", "message_id": "2"}),
                                        ("list_chats", {"cursor": "invalid"})):
                    result = client.call(name, arguments)["result"]
                    assert result["isError"]
                    Draft202012Validator(tools[name]["outputSchema"]).validate(result["structuredContent"])
                invalid = [("list_chats", {"extra": 1}), ("list_chats", {"limit": 101}),
                           ("list_chats", {"cursor": None}), ("get_message", {"chat_id": 1, "message_id": "2"}),
                           ("get_message", {"chat_id": MIN_ID}), ("search_messages", {"query": ""}),
                           ("search_messages", {}), ("search_messages", {"sender": {"type": "user", "id": "1"}}),
                           ("search_messages", {"chat_id": MIN_ID, "query": ""}),
                           ("search_messages", {"chat_id": MIN_ID, "sender": {"type": "user", "id": "-1"}}),
                           ("search_messages", {"chat_id": MIN_ID, "sender": {"type": "bot", "id": "1"}}),
                           ("search_messages", {"chat_id": MIN_ID, "sender": {"type": "chat", "id": MIN_ID, "extra": 1}})]
                for name, arguments in invalid:
                    assert not Draft202012Validator(tools[name]["inputSchema"]).is_valid(arguments)
            if mode == "reactions":
                message = client.call("get_message", {"chat_id": MIN_ID, "message_id": MAX_ID})["result"]["structuredContent"]
                schema = Draft202012Validator(tools["get_message"]["outputSchema"])
                for item in ({"type": "emoji", "emoji": "❤️", "total_count": -1, "is_chosen": False},
                             {"type": "custom_emoji", "custom_emoji_id": 123, "total_count": 1, "is_chosen": False},
                             {"type": "paid", "emoji": "❤️", "total_count": 1, "is_chosen": False},
                             {"type": "emoji", "emoji": "❤️", "total_count": 1},
                             {"type": "unknown", "total_count": 1, "is_chosen": False}):
                    message["reactions"]["items"] = [item]
                    assert not schema.is_valid(message)
        finally:
            client.close()


if __name__ == "__main__":
    validate_schemas()
