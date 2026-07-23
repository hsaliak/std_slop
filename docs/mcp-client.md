# MCP Client Library Plan

This document describes a forward-only plan for a reusable C++ MCP client library in this repo.

The library will support Streamable HTTP MCP servers only. It will not support MCP stdio/stdout transport, and it will not support the deprecated HTTP+SSE transport unless a future requirement asks for it.

## Goals

1. Provide an MCP client library that other projects can depend on from this repo.
2. Let `std::slop` use the same library later for MCP tool support.
3. Reuse existing core infrastructure where appropriate:
   - `core/http_client.h`
   - `core/json_utils.h`
   - `core/oauth_handler.h` patterns where useful
   - Abseil status, time, strings, containers, synchronization, and function utilities
4. Keep MCP protocol parsing out of orchestrator code. The orchestrator should consume a typed MCP API.

## Non-goals

- No stdio/stdout MCP transport.
- No deprecated HTTP+SSE compatibility.
- No full JSON Schema validator in the first version.
- No automatic sampling, roots, or elicitation approval. Those need host policy.
- No provider-specific MCP code inside the generic MCP client library.

## Protocol baseline

Target MCP protocol version: `2025-11-25`.

Required protocol support:

- JSON-RPC 2.0 message encoding and decoding.
- Streamable HTTP transport.
- MCP lifecycle:
  - `initialize`
  - initialize response validation
  - `notifications/initialized`
  - negotiated `protocolVersion`
  - server capability parsing
- Server features:
  - `tools/list`
  - `tools/call`
  - `resources/list`
  - `resources/read`
  - `prompts/list`
  - `prompts/get`
- Utilities:
  - `ping`
  - request timeout
  - `notifications/cancelled`
  - progress notification parsing
  - logging notification parsing
- HTTP authorization hooks:
  - bearer token injection
  - `401 Unauthorized` handling
  - `WWW-Authenticate` parsing for MCP OAuth discovery

## Abseil use

Lean into Abseil for API stability and consistency with the rest of the repo.

Use Abseil types in public MCP APIs when they improve clarity or match existing project style:

- `absl::Status`
- `absl::StatusOr<T>`
- `absl::string_view` for non-owning string inputs
- `absl::Duration` and `absl::Time` for timeouts and expiry
- `absl::flat_hash_map` and `absl::flat_hash_set` for maps and sets
- `absl::Span` for non-owning array inputs if needed
- `absl::StrCat`, `absl::StrAppend`, `absl::StrJoin`, and `absl::Substitute` in implementation code
- `absl::Mutex` and `absl::CondVar` if the HTTP receive path grows a background listener
- `absl::AnyInvocable` or `std::function` for callbacks, depending on what the current Bazel Abseil version supports cleanly
- `absl::variant` only if the current Abseil package exposes it in this repo; otherwise use `std::variant` for JSON-RPC request IDs

Use `std` where it is still the clearer or established choice in this codebase:

- `std::string`
- `std::vector`
- `std::unique_ptr`
- `std::optional` unless we decide to standardize on `absl::optional`

Recommended rule: use Abseil for status, time, string views, containers, and concurrency; use standard library for ownership and sequential containers.

## Proposed package layout

```text
mcp/
  BUILD.bazel

  client.h
  client.cpp

  session.h
  session.cpp

  transport.h
  transport.cpp
  streamable_http_transport.h
  streamable_http_transport.cpp

  json_rpc.h
  json_rpc.cpp

  types.h
  types.cpp

  protocol.h
  protocol.cpp

  authorization.h
  authorization.cpp

  client_test.cpp
  json_rpc_test.cpp
  session_test.cpp
  streamable_http_transport_test.cpp
  protocol_test.cpp
  authorization_test.cpp

  json_rpc_fuzz_test.cpp
  protocol_fuzz_test.cpp
  authorization_fuzz_test.cpp
```

No `stdio_transport.*` files should be added.

## Library layers

### JSON-RPC layer

Files:

- `mcp/json_rpc.h`
- `mcp/json_rpc.cpp`

Responsibilities:

- Build JSON-RPC requests.
- Build JSON-RPC notifications.
- Parse JSON-RPC success responses.
- Parse JSON-RPC error responses.
- Parse server requests and notifications.
- Reject malformed messages before they reach session logic.

Production code must use `core/json_utils.h`:

- `json_parse`
- `json_get`
- `json_get_or`
- `json_dump`

Do not add new raw `nlohmann::json::parse(...)`, `.dump()`, or repeated manual field extraction in handlers, dispatcher code, or orchestrator code.

Suggested types:

```cpp
using JsonRpcId = std::variant<int64_t, std::string>;

struct JsonRpcError {
  int code = 0;
  std::string message;
  nlohmann::json data = nlohmann::json::object();
};

struct JsonRpcResponse {
  JsonRpcId id;
  std::optional<nlohmann::json> result;
  std::optional<JsonRpcError> error;
};
```

If `absl::variant` is available and already enabled in the repo, use it for `JsonRpcId`. If not, keep `std::variant`; do not add a dependency only for that type.

### Transport layer

Files:

- `mcp/transport.h`
- `mcp/streamable_http_transport.h`
- `mcp/streamable_http_transport.cpp`

Base interface:

```cpp
class Transport {
 public:
  virtual ~Transport() = default;

  virtual absl::Status Start() = 0;
  virtual absl::Status Send(const nlohmann::json& message) = 0;
  virtual absl::StatusOr<nlohmann::json> Receive(absl::Duration timeout) = 0;
  virtual absl::Status Close() = 0;
};
```

For a simple synchronous MVP, `Send` can perform the HTTP request and queue the response internally. `Receive` then returns the queued response. If later we need server-initiated messages from GET SSE, the implementation can add a background receive loop without changing the session API.

### Streamable HTTP transport

Responsibilities:

- POST JSON-RPC messages to a single MCP endpoint.
- Include required headers:
  - `Content-Type: application/json`
  - `Accept: application/json, text/event-stream`
  - `MCP-Protocol-Version: <negotiated-version>` after initialize
  - `Mcp-Session-Id: <session-id>` if the server provides one
  - `Authorization: Bearer <token>` when configured
- Parse direct JSON responses.
- Parse SSE responses returned by POST streaming.
- Preserve response order.
- Surface HTTP failures as `absl::Status`.
- Surface JSON-RPC error responses as protocol statuses, not transport failures.

Config:

```cpp
struct StreamableHttpConfig {
  std::string endpoint_url;
  absl::flat_hash_map<std::string, std::string> extra_headers;
  std::optional<std::string> bearer_token;
  absl::Duration request_timeout = absl::Seconds(60);
};
```

Use existing `HttpClient::Post`, `HttpClient::PostStream`, and `HttpClient::Get` where possible.

## Needed core improvements

Streamable HTTP has needs that current `HttpClient` does not fully expose.

Recommended focused additions to `core/http_client.h`:

```cpp
struct HttpResponse {
  long status_code = 0;
  std::string body;
  absl::flat_hash_map<std::string, std::string> headers;
};

absl::StatusOr<HttpResponse> PostWithResponse(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers);

absl::StatusOr<HttpResponse> PostStreamWithResponse(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers,
    ChunkCallback on_chunk);
```

Why this is needed:

- MCP needs `Content-Type` to distinguish JSON from `text/event-stream`.
- MCP can return `Mcp-Session-Id` in response headers.
- MCP authorization discovery uses `401 Unauthorized` plus `WWW-Authenticate`.
- The MCP client must distinguish transport status, response headers, and protocol body.

Existing stream support:

- `HttpClient::PostStream` already delivers body chunks in transport order.
- `interface/interaction_engine.cpp` uses `PostStream` for OpenAI Responses streaming and renders text deltas while the response is still in flight.
- `core/responses_event_decoder.{h,cpp}` already frames SSE-like `data:` events for the OpenAI Responses API, then normalizes OpenAI-specific events into the internal Responses payload shape.
- `core/orchestrator_openai_responses.cpp` also contains an older OpenAI-specific SSE normalization path for completed response bodies.

Recommended SSE improvement:

1. Extract a small generic SSE framer, for example `core/sse_decoder.{h,cpp}` or `mcp/sse_decoder.{h,cpp}`.
2. Keep provider-specific interpretation outside that framer:
   - OpenAI Responses maps SSE payloads to `ResponsesEvent` and normalized Responses JSON.
   - MCP maps SSE payloads to JSON-RPC messages.
3. Prefer putting the generic framer in `core` only if OpenAI Responses and MCP both use it. Otherwise keep it in `mcp`.
4. After the generic framer exists, make `ResponsesEventDecoder` use it and remove the duplicate ad-hoc SSE parser in `orchestrator_openai_responses.cpp`.

Other useful improvements:

1. Add a case-insensitive HTTP header lookup helper in `core/http_client` or the new HTTP response type.
2. Keep auth discovery parsing in `mcp/authorization.*`; do not put MCP-specific authorization semantics into `core/oauth_handler`.
3. Add cancellation support by wiring `Session::Cancel` to `notifications/cancelled` and `HttpClient::Abort()` for local request abort.
4. Let `PostStreamWithResponse` benefit both MCP and OpenAI Responses. OpenAI can use headers/status for better streaming diagnostics, content-type checks, auth failures, and rate-limit metadata.

## Protocol types

Files:

- `mcp/types.h`
- `mcp/protocol.h`

Constants:

```cpp
inline constexpr absl::string_view kJsonRpcVersion = "2.0";
inline constexpr absl::string_view kLatestProtocolVersion = "2025-11-25";
inline constexpr absl::string_view kProtocolVersionHeader = "MCP-Protocol-Version";
inline constexpr absl::string_view kSessionIdHeader = "Mcp-Session-Id";
```

Core types:

```cpp
struct ImplementationInfo {
  std::string name;
  std::string version;
  std::optional<std::string> title;
};

struct ClientCapabilities {
  bool roots = false;
  bool roots_list_changed = false;
  bool sampling = false;
  bool elicitation = false;
  nlohmann::json experimental = nlohmann::json::object();
};

struct ServerCapabilities {
  bool tools = false;
  bool tools_list_changed = false;
  bool resources = false;
  bool resources_subscribe = false;
  bool resources_list_changed = false;
  bool prompts = false;
  bool prompts_list_changed = false;
  bool logging = false;
  nlohmann::json raw = nlohmann::json::object();
};
```

Server feature types:

```cpp
struct Tool {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  nlohmann::json input_schema = nlohmann::json::object();
  nlohmann::json output_schema = nlohmann::json::object();
  nlohmann::json annotations = nlohmann::json::object();
};

struct ToolCallResult {
  std::vector<nlohmann::json> content;
  bool is_error = false;
  nlohmann::json structured_content = nlohmann::json::object();
};

struct Resource {
  std::string uri;
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> mime_type;
};

struct Prompt {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  nlohmann::json arguments = nlohmann::json::array();
};
```

Keep raw JSON fields where useful. This makes the client forward-friendly without adding backward-compatibility branches.

## Session layer

Files:

- `mcp/session.h`
- `mcp/session.cpp`

Responsibilities:

- Own the transport.
- Allocate request IDs.
- Track lifecycle state.
- Send `initialize`.
- Validate initialize response.
- Store negotiated protocol version.
- Store server capabilities.
- Send `notifications/initialized`.
- Provide typed methods:
  - `ListTools`
  - `CallTool`
  - `ListResources`
  - `ReadResource`
  - `ListPrompts`
  - `GetPrompt`
  - `Ping`
  - `Cancel`
- Handle server notifications:
  - `notifications/tools/list_changed`
  - `notifications/resources/list_changed`
  - `notifications/prompts/list_changed`
  - `notifications/progress`
  - logging notifications

Public API sketch:

```cpp
class Session {
 public:
  explicit Session(std::unique_ptr<Transport> transport);

  absl::Status Initialize(const InitializeOptions& options);
  absl::Status Close();

  absl::StatusOr<std::vector<Tool>> ListTools();
  absl::StatusOr<ToolCallResult> CallTool(
      absl::string_view name,
      const nlohmann::json& arguments);

  absl::StatusOr<std::vector<Resource>> ListResources();
  absl::StatusOr<nlohmann::json> ReadResource(absl::string_view uri);

  absl::StatusOr<std::vector<Prompt>> ListPrompts();
  absl::StatusOr<nlohmann::json> GetPrompt(
      absl::string_view name,
      const nlohmann::json& arguments);

  absl::Status Ping();
  absl::Status Cancel(const JsonRpcId& request_id, absl::string_view reason);

  const ServerCapabilities& server_capabilities() const;
  absl::string_view protocol_version() const;
};
```

## Convenience client layer

Files:

- `mcp/client.h`
- `mcp/client.cpp`

Purpose: make the library easy for external consumers.

Example API:

```cpp
absl::StatusOr<std::unique_ptr<Session>> ConnectStreamableHttp(
    const StreamableHttpConfig& config,
    const InitializeOptions& options,
    HttpClient* http_client);
```

The caller can inject `HttpClient` so tests and downstream projects can use fakes.

## Authorization design

### Generic MCP library auth support

The generic MCP client should support:

1. Static bearer token injection.
2. OAuth discovery metadata parsing from HTTP `401 Unauthorized` responses.
3. Token refresh callback hook.
4. Auth-required status that the host can turn into UI or CLI actions.

Suggested types:

```cpp
struct McpAuthMetadata {
  std::string resource_metadata_url;
  std::string authorization_server_url;
  std::string token_endpoint;
  std::string authorization_endpoint;
  std::vector<std::string> scopes_supported;
};

class McpTokenProvider {
 public:
  virtual ~McpTokenProvider() = default;
  virtual absl::StatusOr<std::string> GetAccessToken(
      absl::string_view server_name) = 0;
  virtual absl::Status Refresh(absl::string_view server_name) = 0;
};
```

The MCP library should not open browsers, write token files, or mutate `std::slop` config. It should only report what auth is required and accept tokens from the host.

### `std::slop` MCP auth flow

Add a host-level MCP registry and auth command surface. The command shape should be explicit and scriptable.

Recommended CLI form:

```bash
std_slop mcp add <name> --url <mcp_endpoint> [--auth oauth|bearer|none] [--client-id <id>] [--scope <scope>...]
std_slop mcp remove <name>
std_slop mcp list
std_slop mcp refresh <name>
std_slop mcp login <name>
std_slop mcp logout <name>
```

Command meanings:

- `mcp add`: Add or update an MCP server registry entry. It can probe the endpoint with unauthenticated initialize. If the server returns `401`, parse `WWW-Authenticate` and store discovered auth metadata.
- `mcp remove`: Remove server config and associated token references.
- `mcp list`: Show configured servers, auth state, last refresh time, and last connection status.
- `mcp login`: Start interactive OAuth authorization code + PKCE for one server with browser+paste callback.
- `mcp refresh`: Refresh stored tokens for one server without changing server config.
- `mcp logout`: Delete stored tokens for one server while keeping server config.

Optional later shortcuts:

```bash
std_slop mcp add <name> --url <url> --login
std_slop mcp refresh --all
std_slop mcp remove <name> --purge-tokens
```

In-app slash commands can mirror this later:

```text
/mcp add <name> <url>
/mcp remove <name>
/mcp list
/mcp login <name>
/mcp refresh <name>
/mcp logout <name>
```

But the first implementation should prefer top-level CLI subcommands. They are easier to script and easier to use before an interactive session starts.

### Registry and token storage

Use a split storage model:

1. Store user-authored MCP server registry data in `~/.config/slop/mcp.ini`.
2. Store OAuth tokens outside the registry, one token file per MCP server under `~/.config/slop/mcp/tokens/`.
3. Store optional runtime cache and status in SQLite only if we need queryable audit/debug data later.

Recommended `mcp.ini` shape:

```ini
[server.github]
url = https://example.com/mcp
auth = oauth
enabled = true
scopes = repo read:user

[server.local-metrics]
url = https://metrics.example.com/mcp
auth = bearer
enabled = true
token_path = ~/.config/slop/mcp/tokens/local-metrics.json
```

Why prefer `mcp.ini` for the registry:

- MCP servers are user/workstation configuration, similar to `~/.config/slop/config.ini` and configured `llm_tool_*` sections.
- The active SQLite database can change with `--db` or `--prompt_db`; MCP server configuration should not disappear when a user changes the conversation database.
- Batch mode often uses `:memory:` by default, but MCP server config should still be available.
- INI is easier to inspect, edit, sync, and review than SQLite rows.
- Keeping the registry out of the conversation DB avoids mixing machine config with session ledger data.

What belongs in SQLite later:

- discovered tool cache, if startup latency becomes a problem
- last connection status
- last tool discovery time
- call counts and audit events for MCP tool use
- per-session MCP enable/disable overrides

Do not put bearer or OAuth access tokens in the conversation DB. Tokens should live in separate token files with restrictive permissions. Do not mix MCP tokens with OpenAI tokens.

Possible future cache tables:

```sql
CREATE TABLE mcp_server_status (
  name TEXT PRIMARY KEY,
  last_connected_at INTEGER,
  last_error TEXT,
  last_protocol_version TEXT
);

CREATE TABLE mcp_tool_cache (
  server_name TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  json_schema TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY(server_name, tool_name)
);
```

### Auth flow sequence

1. User runs:

   ```bash
   std_slop mcp add github --url https://example.com/mcp --auth oauth --client-id CLIENT_ID
   ```

2. CLI attempts unauthenticated MCP initialize.
3. Server returns `401 Unauthorized` with `WWW-Authenticate` metadata.
4. CLI fetches protected resource metadata and authorization server metadata.
5. CLI stores discovered `authorization_endpoint` and `token_endpoint` in `~/.config/slop/mcp.ini`.
6. If discovery is unavailable, the user can pass `--authorization-endpoint` and `--token-endpoint` manually.
7. User runs:

   ```bash
   std_slop mcp login github
   ```

8. CLI starts OAuth authorization code + PKCE.
9. CLI stores MCP token under the server name.
10. Later, normal `std::slop` startup loads enabled MCP servers.
11. For each server, `McpTokenProvider` supplies a bearer token.
12. MCP client initializes and lists tools.
13. Orchestrator receives MCP tools as normal callable tools.

### Refresh behavior

- Refresh automatically before use if `expiry_time` is near.
- `std_slop mcp refresh <name>` forces refresh and reports status.
- `std_slop mcp refresh --all` can be added after single-server refresh works.
- If refresh fails, mark server auth state as stale and show a clear login action.

## Later integration with core system

Integration should be a thin adapter layer.

1. Load enabled MCP server configs.
2. For each server:
   - get token from `McpTokenProvider`
   - create `mcp::Session`
   - initialize session
   - call `ListTools`
   - convert each MCP `Tool` into existing internal tool schema
3. Reconcile discovered MCP tools into the existing `tools` table.
4. Register runtime handlers in `ToolExecutor`.
5. When the model requests an MCP tool:
   - route through the existing dispatcher boundary
   - validate tool-call shape before dispatch
   - call `Session::CallTool`
   - normalize MCP content into the existing tool result format
6. Expose resource and prompt support after tool support is stable.

### Tool availability reconciliation

Current tool availability is table-driven:

- `Orchestrator::AssemblePrompt` calls `Database::GetTopLevelTools()`.
- `BuildOpenAiResponsesTools` serializes those rows into model-visible function tools.
- `InteractionEngine` checks tool calls against `GetTopLevelTools()` before dispatch.
- `/tools list` reads top-level rows from the `tools` table.
- `/tools show <name>` reads the selected row from the `tools` table.
- `ToolExecutor` can execute only names registered in its `dispatch_map_`.

Therefore MCP integration must update both surfaces:

1. Register each discovered MCP tool in the `tools` table so it is visible to the model and to `/tools`.
2. Register a matching `ToolExecutor` handler so allowed MCP tool calls can execute.

Naming recommendation for exposed tools:

```text
mcp_<server_name>_<tool_name>
```

The `mcp_` prefix is reserved for runtime-projected MCP tools. Tool names are sanitized to provider-safe characters before projection. A sanitized-name collision or provider-unsafe length fails startup for that server instead of routing to an ambiguous remote tool.

The reconciliation function should be similar to `ReconcileLlmSpecializationTools`:

```cpp
absl::Status ReconcileMcpTools(
    Database* db,
    absl::Span<const McpServerTool> discovered_tools);

absl::Status RegisterMcpToolHandlers(
    ToolExecutor* tool_executor,
    McpSessionRegistry* sessions);
```

Implemented behavior:

- On startup, delete stale rows with the reserved `mcp_` prefix before discovery.
- Upsert discovered MCP tools with `is_enabled = 1` and `is_top_level = 1` by default.
- Convert MCP `inputSchema` into the row `json_schema` used by `BuildOpenAiResponsesTools`.
- Include source metadata in the description, for example: `MCP github: Search repositories`.
- Keep the registry in `mcp.ini`; keep the tool rows as a runtime projection into the active DB.
- If an MCP server is unavailable at startup, remove its projected tool rows for safety and log the startup or auth error.

The high-bar default is to remove tools that cannot be confirmed during startup. Do not expose stale remote tools to the model.

### `/tools` display

The existing `/tools list` will show MCP tools automatically if they are inserted into the `tools` table as top-level tools. That is the lowest-risk first step.

Recommended later improvement:

- Add optional columns to `tools`:
  - `source TEXT DEFAULT 'builtin'`
  - `source_name TEXT`
  - `last_seen_at INTEGER`
  - `status TEXT DEFAULT 'available'`
- Update `/tools list` to show source and status.
- Add `/tools list mcp` or `/mcp tools <server>` as a filtered view.

Example display:

```text
| Name | Source | Description | Enabled | Status |
| `mcp.github.search_repositories` | MCP: github | Search GitHub repositories | ✅ | available |
```

This keeps one canonical tool catalog while still making MCP origin clear.

## Error model

Use `absl::Status` and `absl::StatusOr`.

Suggested mapping:

- malformed JSON-RPC: `absl::InvalidArgumentError`
- protocol violation: `absl::FailedPreconditionError`
- unsupported protocol version: `absl::UnimplementedError`
- timeout: `absl::DeadlineExceededError`
- transport closed: `absl::UnavailableError`
- HTTP 401 requiring auth: `absl::UnauthenticatedError`
- HTTP 403: `absl::PermissionDeniedError`
- JSON-RPC error response: convert with helper and include error code/message

Tool errors are not transport failures. MCP tool errors arrive as successful tool results with `isError: true`. Keep this visible in `ToolCallResult::is_error`.

## Security requirements

- Treat all server messages as untrusted.
- Validate JSON-RPC shape before session side effects.
- Validate tool result shape before converting to internal result.
- Do not execute server-provided data.
- Do not fetch icons with credentials.
- Reject unsafe icon URI schemes if icon fetching is added.
- Require explicit host callbacks for sampling, roots, and elicitation.
- Do not auto-approve elicitation requests.
- Roots are not a security boundary. Real enforcement must be OS sandboxing or file permissions.
- Keep tokens out of logs.
- Redact `Authorization` headers in errors and debug output.

## Implementation status

Implemented on the `mcp-client` branch:

- Bundle 1: JSON-RPC model, protocol types, parser/builder, and validation tests.
- Bundle 2: HTTP response status/header capture and streaming response metadata.
- Bundle 3: Streamable HTTP transport with JSON and SSE POST responses, negotiated protocol/session headers, and bearer token injection.
- Bundle 4: Session initialization, capability negotiation, initialized notification, ping, and typed progress/logging/list-change notification collection.
- Bundle 5: Tools, resources, prompts, and list pagination. Server notifications are collected during requests; hosts can drain them after the request.
- Bundle 6: Authorization metadata parsing and token-provider interface.
- Bundle 7: Registry persistence, secure token storage, OAuth endpoint discovery, OAuth PKCE browser-paste login, refresh/logout, and `std_slop mcp add/remove/list/login/logout/refresh` commands with explicit per-server `client_id`.
- Bundle 8: Runtime tool integration. Enabled registry entries are started during app initialization, tools are discovered and projected as `mcp_<server>_<tool>` top-level tools, calls route through the dispatcher to the correct session, results are normalized, and unavailable servers do not expose stale tools.
- Bundle 9: README plus list-tools and call-tool examples.

Deferred:

- Dynamic Client Registration and OAuth device flow are not supported. Configure a server-provided OAuth `client_id` and use browser-paste authorization-code + PKCE login.
- In-flight request cancellation requires an asynchronous transport/cancellation design and is deferred until that design is available.

## Roadmap

### Bundle 1: JSON-RPC and protocol model

Implement:

- `mcp/json_rpc.{h,cpp}`
- `mcp/types.{h,cpp}`
- `mcp/protocol.{h,cpp}`
- Bazel target `//mcp:mcp`

Tests:

- valid request build
- valid notification build
- valid success response parse
- valid error response parse
- reject missing `jsonrpc`
- reject wrong `jsonrpc`
- reject response with both `result` and `error`
- reject request without method
- request IDs as integer and string

Fuzz:

- random JSON input to JSON-RPC parser must not crash
- malformed shapes rejected cleanly

### Bundle 2: Streamable HTTP response support

Implement focused `HttpClient` improvements:

- `HttpResponse`
- `PostWithResponse`
- `PostStreamWithResponse`
- response header capture exposed to callers

Tests:

- headers captured case-insensitively
- status code exposed
- body preserved
- streaming callback still receives chunks
- retry behavior remains unchanged

### Bundle 3: Streamable HTTP transport

Implement:

- `Transport` interface
- `StreamableHttpTransport`
- required MCP headers
- bearer token injection
- JSON response parsing
- SSE response parsing for POST responses
- `Mcp-Session-Id` tracking

Tests:

- required headers included
- auth header included only when configured
- direct JSON response handled
- SSE response split into JSON-RPC messages
- HTTP 401 returns `Unauthenticated`
- unsupported media type rejected

### Bundle 4: session lifecycle

Implement:

- `Session::Initialize`
- state machine
- request ID allocation
- initialize result parsing
- server capabilities parsing
- initialized notification
- `Ping`

Tests:

- successful initialize
- negotiated version stored
- unsupported version rejected
- methods before initialize rejected
- malformed initialize result rejected
- ping success
- timeout path

### Bundle 5: server feature methods

Implement:

- `ListTools`
- `CallTool`
- `ListResources`
- `ReadResource`
- `ListPrompts`
- `GetPrompt`
- pagination cursor support where required by spec
- list changed, progress, and logging notification parsing

Tests:

- list tools with schemas
- call tool success
- call tool with `isError: true`
- list/read resources
- list/get prompts
- malformed tool/resource/prompt payloads rejected
- pagination continues until no cursor

Fuzz:

- feature result parsing

### Bundle 6: MCP authorization helpers

Implement:

- `mcp/authorization.{h,cpp}`
- `WWW-Authenticate` parser for MCP metadata
- protected resource metadata parser
- authorization server metadata parser
- token provider interface

Tests:

- valid `WWW-Authenticate` parsed
- malformed header rejected cleanly
- missing metadata returns `Unauthenticated`
- metadata JSON shape validated

Fuzz:

- random headers and metadata JSON do not crash

### Bundle 7: `std::slop` MCP registry and CLI design

Implement after the library works:

- server registry storage
- token storage
- `std_slop mcp add/remove/list/login/logout/refresh`
- auth probing and refresh logic

Tests:

- add updates registry
- remove deletes registry entry
- login stores token
- refresh updates token
- list redacts secrets

### Bundle 8: core tool integration

Implement after registry and auth are stable:

- MCP server startup during core initialization
- tool discovery
- internal tool name mapping
- dispatcher adapter
- result normalization
- audit logging

Tests:

- two servers with same tool name do not collide
- tool call routes to correct session
- tool error preserves `isError`
- auth failure gives actionable error
- malformed MCP result does not reach orchestrator as trusted data

### Bundle 9: docs and examples

Add:

```text
mcp/README.md
mcp/examples/list_tools_main.cpp
mcp/examples/call_tool_main.cpp
```

Document:

- Streamable HTTP connection
- auth modes
- lifecycle
- error handling
- security notes
- Bazel dependency example

## Recommended MVP

Build this first:

1. JSON-RPC parser/builder.
2. `HttpClient` response/header support.
3. Streamable HTTP transport.
4. lifecycle initialize.
5. `tools/list`.
6. `tools/call`.
7. docs and one example.

This gives a useful MCP client library without stdio transport work, and it is enough for later `std::slop` tool integration.
