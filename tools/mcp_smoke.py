import argparse
import asyncio
import importlib.metadata
import sys

from mcp_profile import children, describe, measure, server_pid

from jsonschema import Draft202012Validator, FormatChecker
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def smoke(arguments):
    if importlib.metadata.version("mcp") != "1.12.4":
        raise RuntimeError("Install tools/mcp-client-requirements.txt")
    server_arguments = [arguments.fixture] if arguments.fixture else ["mcp", "serve", arguments.config]
    parameters = StdioServerParameters(command=arguments.server, args=server_arguments)
    previous = children() if arguments.profile_build else set()
    async with stdio_client(parameters) as (reader, writer):
        async with ClientSession(reader, writer) as session:
            initialized = await session.initialize()
            assert initialized.protocolVersion == "2025-06-18"
            pid = None
            if arguments.profile_build:
                pid = server_pid(previous, arguments.server)
                describe(arguments.profile_build, arguments.fixture)
                async with measure(pid, "idle"):
                    await asyncio.sleep(1)
            discovered = await session.list_tools()
            tools = {tool.name: tool for tool in discovered.tools}
            assert set(tools) == {"list_chats", "search_messages", "get_message"}
            calls = [
                ("list_chats", {"limit": 1}),
                ("search_messages", {"query": arguments.query, "chat_id": arguments.chat_id, "limit": 1}),
                ("get_message", {"chat_id": arguments.chat_id, "message_id": arguments.message_id}),
            ]
            for name, inputs in calls:
                tool = tools[name]
                Draft202012Validator.check_schema(tool.inputSchema)
                Draft202012Validator.check_schema(tool.outputSchema)
                Draft202012Validator(tool.inputSchema, format_checker=FormatChecker()).validate(inputs)
                if pid and name == "search_messages":
                    async with measure(pid, "search"):
                        result = await session.call_tool(name, inputs)
                else:
                    result = await session.call_tool(name, inputs)
                Draft202012Validator(tool.outputSchema, format_checker=FormatChecker()).validate(result.structuredContent)
                if result.isError:
                    raise RuntimeError(name + " failed: " + result.structuredContent["error"]["code"])
                if name == "search_messages" and result.structuredContent.get("error"):
                    raise RuntimeError("Search was incomplete with an error")
                print(name + ": passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Official Python MCP SDK 1.12.4 stdio smoke client; never prints Telegram data.")
    parser.add_argument("--server", required=True, help="Absolute path to telegram-mcpp")
    parser.add_argument("--config", help="Absolute path to protected configuration")
    parser.add_argument("--chat-id", required=True)
    parser.add_argument("--message-id", required=True)
    parser.add_argument("--query", required=True)
    parser.add_argument("--profile-build", help="Record idle/search Linux RSS and CPU; supply exact build settings")
    parser.add_argument("--fixture", help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if not arguments.config and not arguments.fixture:
        parser.error("--config is required")
    try:
        asyncio.run(smoke(arguments))
    except Exception:
        print("MCP smoke test failed.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
