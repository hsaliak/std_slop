# Simple MCP Server: Implementation Plan

## Goal

Add the smallest useful C++ MCP server to this repository. It uses **stdio only**, implements **only the current MCP protocol revision `2026-07-28`**, and exposes tools. It is a new server package, separate from the existing MCP client.

The current revision is listed as `2026-07-28` in the official MCP versioning documentation. Treat that literal version as the supported version for this implementation. Do not add the legacy `initialize` handshake, legacy protocol support, version fallback, Streamable HTTP, or authentication as part of this plan.

## Existing code and constraints

- `mcp/client.*`, `mcp/session.*`, and `mcp/streamable_http_transport.*` implement outbound MCP client behavior.
- `mcp/modern.*` implements modern client-side request encoding and response parsing. It can inform wire formats, but it is not a server dispatcher.
- `mcp/json_rpc.*` has shared JSON-RPC helpers.
- `mcp/types.h` contains existing protocol data types. Reuse types only where their meaning is shared; do not expose client-session abstractions as server APIs.
- `mcp/json_schema.*` validates a bounded JSON Schema 2020-12 subset and may be used to check tool schemas and arguments.
- `mcp/runtime.*` is an outbound client runtime that registers remote tools in `std_slop`. It is not the new server runtime.
- Production JSON parsing, access, and dumping must use `core/json_utils.h` helpers.
- Add or update Bazel targets for each new source, test, or example.

## Protocol and transport rules

The server implements the current `2026-07-28` protocol only:

- Every protocol request declares its protocol version in `_meta.io.modelcontextprotocol/protocolVersion`.
- The server supports the mandatory `server/discover` request and advertises only `2026-07-28`.
- The initial feature surface is `tools/list` and `tools/call`.
- There is no `initialize` / `notifications/initialized` lifecycle in this protocol version.
- Unsupported protocol versions return the current protocol's unsupported-version error with the supported version listed. Do not silently handle such a request under another version.

For stdio:

- Read one UTF-8 JSON-RPC message per input line; write one JSON-RPC message per output line.
- Do not put diagnostics, startup banners, or other text on stdout. Write diagnostics to stderr.
- Handle EOF as normal shutdown.
- Do not implement an HTTP listener, TLS, OAuth, or token handling.

## Proposed package layout

```text
mcp/server/
  BUILD.bazel
  server.h / server.cpp                 # public server API, registration, dispatch
  stdio.h / stdio.cpp                   # line-oriented stdio loop
  server_test.cpp                       # in-process protocol and dispatch tests
  stdio_test.cpp                        # stream-level framing and I/O tests
  server_fuzz_test.cpp                  # malformed JSON-RPC / dispatch inputs
  echo_server.cpp                       # runnable stdio example
```

The server API should be transport-independent at the dispatch boundary. The stdio runner should accept input and output streams so unit tests can exercise the real framing and dispatch path without launching a child process. The example executable supplies `std::cin`, `std::cout`, and `std::cerr`.

## Implementation bundles

### Bundle 1: Server API and tool registry

**Implementation**

- Define a small `Server` type that owns server identity and a registry of registered tools.
- Provide a registration operation with a tool definition and a C++ handler. A handler receives validated arguments and returns a typed tool result or an `absl::Status` that the dispatcher can map to a protocol error.
- Keep registration and dispatch separate from input/output. Do not add a generic plugin system, network abstraction, async task system, or client dependency.
- Reject invalid or duplicate tool names, malformed tool schemas, empty handlers, and invalid identity data at registration time.
- Use shared MCP types and JSON Schema helpers only where they fit their existing contracts.

**Tests and acceptance checks**

- Test registering one valid tool and listing its public definition.
- Test invalid names, duplicate names, invalid schemas, and missing handlers.
- Test that one bad registration does not leave a partial registry entry.
- Validate the package and test target with Bazel.

### Bundle 2: Latest-version protocol dispatch

**Implementation**

- Add a JSON-RPC dispatcher for `server/discover`, `tools/list`, and `tools/call`.
- Parse and validate the JSON-RPC envelope and request metadata before dispatch. Use `json_parse`, `json_get`, `json_get_or`, and `json_dump` from `core/json_utils.h` in production code.
- Make `server/discover` advertise the exact supported version (`2026-07-28`), server identity, and the tools capability. Do not advertise resources, prompts, subscriptions, or other unimplemented capabilities.
- Require the current protocol version in request metadata and reject unsupported or missing versions with the protocol-defined version error. No legacy interpretation or fallback is allowed.
- `tools/list` returns the registered tools in the current protocol's response shape.
- `tools/call` validates method parameters, tool name, arguments object, and arguments against the registered input schema before invoking the handler. Invalid input must not reach the handler.
- Return tool execution results in the current protocol's tool-result shape. Distinguish protocol/dispatch errors from tool-level errors using the current protocol's rules.
- Handle notifications without sending a JSON-RPC response when required by JSON-RPC. Do not emit server-initiated requests or notifications in this first version.

**Tests and acceptance checks**

- Add fixture-based tests for current-version discovery, tool listing, valid tool calls, tool-level errors, and unknown tools.
- Test malformed envelopes, missing or invalid request metadata, unsupported protocol version, missing parameters, non-object arguments, schema-invalid arguments, and unknown methods.
- Assert invalid calls do not invoke the registered handler.
- Test JSON-RPC request IDs are preserved in responses and notifications do not produce responses.
- Add a fuzz target for untrusted JSON-RPC input. It must not crash, must reject malformed shapes cleanly, and must never invoke a handler for malformed or version-incompatible requests.

### Bundle 3: stdio transport loop

**Implementation**

- Implement a blocking stream loop that reads one line, dispatches one message, and writes the resulting message as one JSON line.
- Make stream objects injectable for tests. The production example wires stdin/stdout/stderr to the loop.
- Keep stdout protocol-only. Write parse errors, operational diagnostics, and startup failures to stderr.
- Define and enforce a reasonable maximum input-line size to prevent unbounded memory growth. Treat an over-limit line as a clean protocol/input failure and do not invoke a tool.
- Flush each response so a host does not wait for buffered output.
- Exit cleanly on EOF. Do not add background threads or asynchronous I/O for this MVP.

**Tests and acceptance checks**

- Test multiple request lines in one stream, response line framing, output JSON parsing, EOF, blank/malformed lines, and the input-line limit.
- Verify logs go only to the supplied error stream and response output contains no non-protocol text.
- Run tests with piped input to prove there is no TTY dependency.

### Bundle 4: runnable echo server

**Implementation**

- Add a small executable that registers an `echo` tool with a simple JSON input schema and returns its input as a result.
- Keep the example deterministic and side-effect free.
- The executable must write no banner to stdout; usage or fatal-error text goes to stderr.
- Document the build and run commands in this plan or the MCP documentation once the executable exists.

**Tests and acceptance checks**

- Build the example target with Bazel.
- Add a process-level test or scripted smoke test that starts the binary, writes `server/discover`, `tools/list`, and `tools/call` requests to stdin, and parses the corresponding stdout responses.
- Verify stderr output does not contaminate the protocol stream.

### Bundle 5: Public docs and scope guard

**Implementation**

- Add a short server section to the MCP documentation that distinguishes the inbound server package from the existing outbound client and runtime.
- Document the supported protocol version, stdio framing, example commands, and unsupported features.
- State clearly that stdio needs no MCP OAuth flow and that HTTP/auth are out of scope for this implementation.

**Tests and acceptance checks**

- Run the relevant server unit tests, fuzz target smoke test, and example build.
- Run formatting checks for all new C++ files and documentation review for consistency.
- Search the new server package for accidental stdout logging and for legacy protocol/version fallback paths.

## Definition of done

- A host can launch the example as a subprocess and exchange newline-delimited MCP messages over stdio.
- The example passes `server/discover`, lists its `echo` tool, and successfully calls it.
- The server accepts only protocol version `2026-07-28` and does not implement the legacy initialize flow.
- Invalid protocol and tool inputs fail without crashes or unintended handler execution.
- Stdout contains only valid MCP JSON-RPC messages; diagnostics go to stderr.
- New production code, tests, fuzz tests, and example targets are present in Bazel and pass their focused validations.

## References

- [MCP current version and negotiation](https://modelcontextprotocol.io/docs/2026-07-28/learn/versioning.md)
- [MCP stdio transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio.md)
- [MCP tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools.md)
- Repository client API notes: [`mcp-api.md`](mcp-api.md)

> Protocol URLs above use the current revision listed by the official MCP docs when this plan was written. If the official current revision changes before implementation, update the version scope and fixtures deliberately before coding; do not silently add multi-version compatibility.

