# Simple MCP Server: Implementation Plan

## Goal

Add the smallest useful C++ MCP server to this repository. It uses **stdio only**, implements **only the current MCP protocol revision `2026-07-28`**, and exposes tools. It is a new server package, separate from the existing MCP client.

The current revision is listed as `2026-07-28` in the official MCP versioning documentation. Treat that literal version as the supported version for this implementation. Do not add the legacy `initialize` handshake, legacy protocol support, version fallback, Streamable HTTP, or authentication as part of this plan.

## Existing code and constraints

- `mcp/client/client.*`, `mcp/client/session.*`, and `mcp/client/streamable_http_transport.*` implement outbound MCP client behavior.
- `mcp/client/modern.*` implements modern client-side request encoding and response parsing. It can inform wire formats, but it is not a server dispatcher.
- `mcp/json_rpc.*` has shared JSON-RPC helpers.
- `mcp/types.h` contains existing protocol data types. Reuse types only where their meaning is shared; do not expose client-session abstractions as server APIs.
- `mcp/json_schema.*` validates a bounded JSON Schema 2020-12 subset and may be used to check tool schemas and arguments.
- `mcp/client/runtime.*` is an outbound client runtime that registers remote tools in `std_slop`. It is not the new server runtime.
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
  tools_test.cpp                        # registry, call and output validation tests
  tools_fuzz_test.cpp                   # malformed tool inputs and handler gating
  stdio_test.cpp                        # stream-level framing and I/O tests
  stdio_fuzz_test.cpp                   # framing limits and execution gating
  server_fuzz_test.cpp                  # malformed JSON-RPC / dispatch inputs
  echo_server.cpp                       # runnable stdio example
  echo_server_test.py / .sh              # subprocess RPC integration harness
```

The server API should be transport-independent at the dispatch boundary. The stdio runner should accept input and output streams so unit tests can exercise the real framing and dispatch path without launching a child process. The example executable supplies `std::cin`, `std::cout`, and `std::cerr`.

## Implementation bundles

### Bundle 1: Latest-only request dispatch and discovery

**Status:** Implemented and verified. `bazel test //mcp/server:all //mcp:all //mcp/client:all` passes. Tool registration is added in Bundle 2; stdio execution remains in Bundle 3.

**Implementation**

- Define `slop::mcp::server::Server` with validated identity and transport-independent `Dispatch(raw)` returning an optional JSON-RPC response.
- Validate the JSON-RPC envelope, ID, method, parameters and version metadata before dispatch. Reject client response messages at the inbound boundary.
- Implement `server/discover` with `resultType: complete`, `supportedVersions`, implemented capabilities and `_meta.io.modelcontextprotocol/serverInfo`.
- Support only `2026-07-28`. Unsupported string versions produce error `-32022` with `data.supported` and `data.requested`; absent or malformed version metadata produces invalid-params errors.
- Bundle 1 starts with no capabilities. Bundle 2 adds the tools capability. Unknown methods return method-not-found; valid notifications produce no response.
- Keep protocol errors as JSON-RPC messages for stdout once a transport exists. Operational diagnostics belong on stderr.
- Do not add I/O, client dependencies, SQLite, ToolExecutor, UI or orchestrator dependencies. Server response construction stays local until another production consumer requires a shared helper.

**Tests and acceptance checks**

- Unit tests cover discovery, IDs and numeric bounds, metadata, parse errors, malformed envelopes, unsupported versions, unknown methods and notifications.
- Check discovery against the existing modern client codec in tests only.
- Add deterministic, side-effect-free fuzz coverage for raw input and structured malformed metadata. Assert rejected inputs cannot produce successful discovery results.
- Update the package BUILD and run server tests plus shared/client regression tests.

### Bundle 2: Tool registration and execution

**Status:** Implemented and verified with `bazel test //mcp/server:all //mcp:all //mcp/client:all`. Tool registration uses an immutable registry; stdio remains out of scope for this bundle.

**Implementation**

- Extend the dispatcher with `tools/list` and `tools/call`. `Server::Create(identity, vector<ToolRegistration>)` validates all definitions and handlers and freezes the registry by construction. Invalid or duplicate names, malformed schemas and empty handlers fail creation without an observable partial registry.
- Parse and validate the JSON-RPC envelope and request metadata before dispatch. Use `json_parse`, `json_get`, `json_get_or`, and `json_dump` from `core/json_utils.h` in production code.
- Make `server/discover` advertise the exact supported version (`2026-07-28`), server identity, and the tools capability. Do not advertise resources, prompts, subscriptions, or other unimplemented capabilities.
- Keep Bundle 1 version and envelope validation. No legacy interpretation or fallback is allowed.
- `tools/list` returns registered tools in a stable order. A small fixed catalog does not need pagination initially.
- `tools/call` validates method parameters, tool name and object arguments before invoking the handler. Missing optional arguments default to an empty object. Schema-invalid arguments return actionable `isError` results without invoking the handler. Malformed call shapes and unknown tools return JSON-RPC `-32602` errors; unsupported versions keep Bundle 1 behavior.
- Return complete tool results with validated content block shapes and object metadata. Successful structured results must satisfy the declared output schema. The shared `Tool` type's empty output-schema object means no declared schema. Explicit tool failures may omit structured content; any provided structured error content is still schema-checked. Handler Status failures or malformed results return JSON-RPC `-32603` without exposing internal status details.
- Only synchronous complete results are supported. Reject supplied cursors, `inputResponses` and `requestState`; do not execute resumable calls. Handlers must report errors through Status/result fields, consistent with exception-disabled C++ builds.
- Harden shared `CheckJsonSchema` keyword shape checks at registration rather than duplicating generic schema validation in server code. Unit/fuzz tests cover invalid type names and supported keyword shapes.
- Handle notifications without sending a JSON-RPC response when required by JSON-RPC. Do not emit server-initiated requests or notifications in this first version.

**Tests and acceptance checks**

- Add fixture-based tests for current-version discovery, tool listing, valid tool calls, tool-level errors, and unknown tools.
- Test malformed envelopes, missing or invalid request metadata, unsupported protocol version, missing parameters, non-object arguments, schema-invalid arguments, and unknown methods.
- Assert invalid calls do not invoke the registered handler.
- Test JSON-RPC request IDs are preserved in responses and notifications do not produce responses.
- Add a fuzz target for untrusted JSON-RPC input. It must not crash, must reject malformed shapes cleanly, and must never invoke a handler for malformed or version-incompatible requests.

### Bundle 3: stdio transport loop

**Status:** Implemented and verified. `bazel test //mcp/server:all //mcp:all //mcp/client:all` covers the transport, dispatch, client regressions and formatting. The runnable server and subprocess tests are supplied by Bundle 4.

**Implementation**

- Implement a blocking stream loop that reads one line, dispatches one message, and writes the resulting message as one JSON line.
- `RunStdio(server, input, output, diagnostics, options)` uses borrowed streams and returns `absl::Status`; no global stream manipulation, TTY checks, process launch or stream closing is performed. The Bundle 4 example supplies stdin/stdout/stderr. Streams must use separate buffers and have exception masks disabled; unsafe configurations fail before I/O.
- Keep stdout protocol-only. Send protocol parse/request errors as JSON-RPC responses; write diagnostic logs, operational failures and startup failures to stderr.
- Enforce `StdioOptions::max_input_bytes` while reading, not after unbounded `std::getline`. The default is 1 MiB per line excluding LF; CR is counted, so CRLF is accepted when it fits the limit. Zero is invalid. Oversized input stops with `ResourceExhausted` and a best-effort diagnostic, without dispatching that line or draining further input. Handler-produced response sizes are not limited by this transport.
- Flush each response before reading the next request. Read/write/flush failures stop the loop with an `Internal` status. Failed diagnostics never mask the primary status. Output failure may occur after handler side effects; callers must not automatically replay requests.
- EOF with no pending bytes is normal shutdown. EOF before LF terminates with `DataLoss`, logs a diagnostic and never dispatches the unterminated line, even if its JSON is otherwise complete. Do not add background threads or asynchronous I/O.

**Tests and acceptance checks**

- Test multiple request lines in one stream, response line framing, output JSON parsing, EOF, blank/malformed lines, and the input-line limit.
- Verify logs go only to the supplied error stream and response output contains no non-protocol text.
- Unit coverage includes a real OS pipe redirected to stdin to prove there is no TTY dependency. Bundle 4 provides process-level example tests.
- Fuzz a deterministic framing reference model and assert oversized/truncated calls never reach handlers.

### Bundle 4: runnable echo server

**Status:** Implemented as a subsequent patch to Bundle 3. `//mcp/server:echo_server` is a deterministic fixture and reusable example using the real dispatcher and transport. `//mcp/server:echo_server_test` launches it over real subprocess pipes with an independent Python standard-library client.

**Implementation**

- Add a small executable that registers an `echo` tool with a simple JSON input schema and returns its input as a result.
- Keep the example deterministic and side-effect free.
- Untie stdin from stdout so the integration test verifies explicit transport flushes while stdin remains open. Ignore SIGPIPE in the executable so closed stdout is reported as an I/O status/diagnostic rather than terminating by signal.
- The executable must write no banner to stdout; usage or fatal-error text goes to stderr.
- Document the build and run commands in this plan or the MCP documentation once the executable exists.

**Tests and acceptance checks**

- Build the example target with Bazel.
- Add a process-level test or scripted smoke test that starts the binary, writes `server/discover`, `tools/list`, and `tools/call` requests to stdin, and parses the corresponding stdout responses.
- Verify stderr output does not contaminate the protocol stream. Use raw subprocess pipes: the current outbound MCP client is HTTP-only.
- The black-box test covers live discovery/list/call, escaped text, silent notifications, protocol errors and recovery, quiet EOF, usage, truncated/oversized input, and closed stdout. Reply reads have deadlines and size bounds; child processes are killed/reaped on test failure. Requires Python 3 and standard library only.
- This is end-to-end coverage of the server executable, not `std_slop` runtime connectivity: the existing client and registry support HTTP endpoints only. Launching this fixture through `std_slop` needs a separate outbound stdio-client feature.

**Build and run**

```sh
bazel build //mcp/server:echo_server
bazel test --nocache_test_results //mcp/server:echo_server_test
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | bazel-bin/mcp/server/echo_server
```

The example accepts no configuration arguments. `--help` prints usage to stderr. No API keys, model calls, database, or network services are needed.

### Bundle 5: Public docs and scope guard

**Implementation**

- Add a short server section to the MCP documentation that distinguishes the inbound server package from the existing outbound client and runtime.
- Document the supported protocol version, stdio framing, example commands, and unsupported features.
- State clearly that stdio needs no MCP OAuth flow and that HTTP/auth are out of scope for this implementation.

**Tests and acceptance checks**

- Run the relevant server unit tests, fuzz target smoke test, and example build.
- Run formatting checks for all new C++ files and documentation review for consistency.
- Search the new server package for accidental stdout logging and for legacy protocol/version fallback paths.

## Application integration boundary

Do not embed `ToolExecutor` in the reusable server library or automatically export all agent tools. A follow-up app adapter must explicitly allowlist tools, define workspace/session ownership, preserve argument validation and mail protections, and avoid stdout contamination. Shell execution, unrestricted SQL and Git mutation require separate policy decisions. Stdio needs no MCP OAuth flow but does not remove local permission requirements.

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

