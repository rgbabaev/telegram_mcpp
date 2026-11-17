from jsonschema import Draft202012Validator, FormatChecker
from mcp_stdio import Client, MIN_ID, MAX_ID


def validate_schemas():
    for mode in ("normal", "partial", "rate", "unavailable", "maximal", "revoked"):
        client = Client(mode)
        try:
            client.initialize()
            tools = {tool["name"]: tool for tool in client.request("tools/list")["result"]["tools"]}
            calls = [("list_chats", {}), ("search_messages", {"query": "private-query", "limit": 100}),
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
                           ("get_message", {"chat_id": MIN_ID}), ("search_messages", {"query": ""})]
                for name, arguments in invalid:
                    assert not Draft202012Validator(tools[name]["inputSchema"]).is_valid(arguments)
        finally:
            client.close()


if __name__ == "__main__":
    validate_schemas()
