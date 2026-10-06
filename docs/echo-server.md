# Echo MCP server

`echo_server` is a small, deterministic MCP server used in examples and tests. It is one of the MCP implementations in this repository. For the C++ server library and its full API, see the [MCP server guide](mcp-server.md).

## Tool

The server exposes one tool, `echo`:

- Input: an object with a required string field, `text`.
- Result: the supplied text as both text content and `structuredContent`.
- Annotation: read-only; the handler has no side effects.

The server uses stdio and MCP protocol version `2026-07-28`. It does not need an HTTP listener, model credentials, or a database.

## Build and test

```sh
bazel build //mcp/server:echo_server
bazel test //mcp/server:echo_server_test
```

## Use with the run_js gateway

The echo server is also the example downstream MCP for the [run_js gateway](../mcp/gateway/README.md). Configure its executable path as a stdio server and allow the `echo` tool:

```json
{
  "servers": [
    {
      "alias": "echo",
      "transport": "stdio",
      "command": "/absolute/path/to/bazel-bin/mcp/server/echo_server",
      "args": [],
      "allowTools": ["echo"]
    }
  ]
}
```

With that configuration, JavaScript can call `await echo.echo({text: "hello"})` through the gateway. The gateway's [setup guide](../mcp/gateway/README.md) covers registration and execution.
