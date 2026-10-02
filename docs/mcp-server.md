# C++ MCP stdio server

The `mcp/server/` package implements an inbound MCP server. It is separate from the outbound client in `mcp/client/` and the client runtime that imports remote tools into `std_slop`.

The server supports **protocol version `2026-07-28` only**, over newline-delimited JSON-RPC 2.0 on stdin/stdout. It has no HTTP listener, TLS configuration, OAuth flow, database requirement, or model dependency.

For the outbound client, see [mcp-api.md](mcp-api.md). For implementation history and acceptance checks, see [mcp-server-simple.md](mcp-server-simple.md).

## Server quick start

Build the deterministic echo example and run its subprocess integration tests:

```sh
bazel build //mcp/server:echo_server
bazel test --nocache_test_results //mcp/server:echo_server_test
```

Send one tool call to the example:

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | bazel-bin/mcp/server/echo_server
```

The reply contains `resultType: "complete"`, text content, and `structuredContent: {"text":"hello"}`. Each request must end with LF, including the last request before closing stdin. The example accepts no configuration arguments; `--help` writes usage to stderr. Its exit codes are:

| Exit code | Meaning |
| --- | --- |
| `0` | Clean EOF, or `--help`. Protocol/tool errors can still have been returned. |
| `1` | Startup or transport failure; diagnostics go to stderr. |
| `2` | Invalid command-line arguments; usage goes to stderr. |

Use the built binary as a host-launched subprocess. **Do not register it with `std_slop mcp add`: that client currently supports HTTP endpoints, not stdio commands.** The integration test uses an independent Python standard-library client and real pipes; it is not a `std_slop` runtime integration. Python 3 is needed for that test, not for the C++ server binary.

## Server package and API

| Component | Purpose |
| --- | --- |
| `mcp/server/server.h`, `//mcp/server:server` | Immutable tool registry and transport-independent request dispatch. |
| `mcp/server/stdio.h`, `//mcp/server:stdio` | Blocking stream runner with bounded input framing. |
| `mcp/protocol.h`, `mcp/types.h` | Shared protocol definitions and data types. |
| `mcp/json_schema.*` | Shared bounded JSON Schema validation. |
| `mcp/server/echo_server.cpp` | Complete executable example using these APIs. |

To build an application:

1. Set nonempty `ImplementationInfo::name` and `version`.
2. Assemble a vector of `ToolRegistration` entries. Each entry holds a `Tool` definition and a `ToolHandler`.
3. Call `Server::Create(identity, registrations)`. This validates all registrations and freezes the registry. Failure returns an `absl::Status`; no partially registered server is exposed.
4. Either call `Server::Dispatch(raw)` directly, or pass the server and borrowed streams to `RunStdio(server, input, output, diagnostics, options)`.

`Dispatch` returns an optional JSON-RPC response. A valid notification returns no response and does not execute a tool. The server library has no client, UI, orchestrator, `ToolExecutor`, or SQLite dependency.

### Tool registration and handler contract

- Tool names contain 1–128 ASCII letters, digits, `_`, `-`, or `.`. Names are case-sensitive and must be unique. Catalogs are sorted by name.
- Handlers must be nonempty. They accept a const JSON argument object and return `absl::StatusOr<ToolCallResult>`.
- Input schemas must be objects describing object arguments. Schemas, annotations, and metadata are checked during creation. Validation uses the supported JSON Schema 2020-12 subset with bounded work/depth and local references only; it is not a complete JSON Schema implementation.
- The shared `Tool` type uses an empty `output_schema` object to mean no declared output schema. A nonempty output schema requires conforming structured content on successful results.
- A handler must return a complete result. Basic text, image, audio, resource-link and embedded-resource content shapes and object metadata are checked. Structured content may be an object, array, scalar, or JSON null when its schema permits it.
- Use `ToolCallResult::is_error` for actionable tool failures. Such failures may omit structured output; any supplied structured error output is still checked against the declared schema.
- A non-OK handler Status or malformed result becomes a JSON-RPC server error without exposing internal status details. Report errors through Status/result fields; the project builds C++ with exceptions disabled.
- Handlers own their side effects and must not print directly to the protocol output stream. Applications must enforce their own access policy, resource budgets, and execution limits.

## Supported server protocol

| Method | Behavior |
| --- | --- |
| `server/discover` | Reports only `2026-07-28`, server identity, and the tools capability. |
| `tools/list` | Returns the fixed tool catalog; supplied cursors are rejected. |
| `tools/call` | Validates the request and arguments before handler invocation. Missing arguments default to `{}`. |

Every request declares the version in `params._meta["io.modelcontextprotocol/protocolVersion"]`. Request IDs must be non-null strings or integers within the JSON safe-integer range, from `-9007199254740991` to `9007199254740991`. There is no legacy `initialize` / `notifications/initialized` handshake and no version fallback. Discovery is useful but is not required before a tool call.

Protocol and tool errors are ordinary responses; they do not stop the stdio loop:

| Error | Result |
| --- | --- |
| Malformed JSON, including a blank line | JSON-RPC `-32700`. |
| Invalid request envelope or ID | JSON-RPC `-32600`. |
| Unknown method | JSON-RPC `-32601`. |
| Malformed/missing metadata, malformed call, unknown tool, cursor or resumption fields | JSON-RPC `-32602`. |
| Unsupported nonempty string protocol version | JSON-RPC `-32022`, with `data.supported` and `data.requested`. |
| Schema-invalid arguments | Complete tool result with `isError: true`; handler is not called. |
| Handler Status failure or invalid result | JSON-RPC `-32603`. |

## Stdio framing and operational limits

`RunStdio` uses borrowed streams; it does not launch processes, inspect a TTY, change global streams, or close the supplied streams. Input, output and diagnostic streams must have separate buffers and exception masks disabled.

| Condition | Runner behavior |
| --- | --- |
| Complete LF-delimited line | Dispatch it and flush any response before reading the next line. |
| CRLF | Accepted; CR counts toward the input limit. |
| Input limit | `StdioOptions::max_input_bytes` defaults to 1 MiB per line, excluding LF. Zero is invalid. The bound is enforced while reading. |
| Oversized line | Stop with `ResourceExhausted`; do not dispatch that line or drain later input. |
| EOF with no pending bytes | Return OK. |
| EOF before LF | Stop with `DataLoss`; do not dispatch the unterminated line, even if the JSON is complete. |
| Read/write/flush failure | Stop with an `Internal` status. |
| Operational diagnostic failure | Preserve the primary status; diagnostic logging is best-effort. |
| Unsafe stream configuration | Return `InvalidArgument` before I/O. |

Protocol messages go only to output; operational diagnostics go to the diagnostic stream. Diagnostics do not contain the rejected input payload. Handler-produced response sizes are not bounded by this transport.

An output failure may occur after a tool has run. **Do not automatically replay a request after transport failure.** The echo executable ignores SIGPIPE to let closed stdout become an I/O status; the reusable library does not install signal handlers.

## Server scope and security

This implementation has no HTTP transport, authentication endpoints, resources/prompts APIs, subscriptions, pagination, resumable calls, background execution, built-in handler timeouts, or cancellation.

Stdio needs no MCP OAuth flow, but it is not a permission boundary. The host launches a local process with local permissions and environment. Treat incoming arguments as untrusted and define tool access policy explicitly.

The example exposes only `echo`. It does not automatically expose agent tools. A future application adapter must allowlist tools, define workspace/session ownership, preserve existing argument validation and mail-mode protections, and keep stdout clean. Shell execution, unrestricted SQL, and Git mutation require separate policy decisions.

## Server validation commands

```sh
bazel test --nocache_test_results //mcp:all //mcp/client:all //mcp/server:all
```

The server package includes exact unit tests, deterministic fuzz smoke tests, formatting checks, an OS-pipe unit test and independent subprocess integration tests. The process tests cover live discovery/catalog/calls, explicit flushing while stdin is open, notifications, error recovery, EOF, input limits, usage and closed stdout. They require no network service, model access, API key or database.
