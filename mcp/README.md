# MCP client

This package provides a bounded Model Context Protocol (MCP) client for Streamable HTTP servers. It supports the classic `2025-11-25` revision and the modern `2026-07-28` revision.

## Connect and select a revision

Use `ConnectMcp` for normal operation. It probes modern MCP with `server/discover`. Under `kPreferLatest`, it falls back to classic MCP when discovery shows that modern MCP is unsupported: an HTTP 400 without a JSON-RPC error, an HTTP 200 JSON-RPC `-32601` method-not-found response, an HTTP 400 JSON-RPC `-32000` error whose message says `unsupported protocol version`, or a successful discovery response that does not advertise `2026-07-28`. Choose `kClassicOnly` to skip modern discovery or `kLatestOnly` to require modern MCP without fallback.

```c++
slop::mcp::StreamableHttpConfig config;
config.endpoint_url = "https://example.com/mcp";

slop::mcp::ClientOptions options;
options.selection = slop::mcp::SelectionPolicy::kPreferLatest;
options.client_info.name = "my-client";
options.client_info.version = "1.0";

slop::HttpClient http_client;
auto client = slop::mcp::ConnectMcp(config, options, &http_client);
```

For direct classic use, call `ConnectClassicStreamableHttp` with `v2025_11_25::InitializeOptions`.

## Supported operations

Classic sessions support `Ping`, tool/resource/prompt listing and calls, resource reads, and resource subscriptions when the server advertises `resources.subscribe`. Resource updates are available through `DrainNotifications()`.

Modern clients support bounded, paginated tool catalogs, schema validation, structured tool results, and resumable tool calls. If a result has `kind == ToolResultKind::kInputRequired`, pass its `request_state` to `ContinueToolCall` after collecting the required input.

Modern tool and result metadata is preserved as opaque JSON. Unknown notification methods are ignored; malformed messages, duplicate catalog entries, cursor cycles, and resource-limit violations are rejected.

## Authorization and token storage

`authorization.h` contains helpers for protected-resource OAuth discovery:

- `ParseWwwAuthenticateResourceMetadata()` extracts and validates the `resource_metadata` URL from a Bearer challenge.
- `ParseProtectedResourceMetadata()` and `ParseAuthorizationServerMetadata()` validate discovery documents.
- `ParseClientIdMetadataDocument()` validates HTTPS Client ID Metadata Documents for public clients.
- `ValidateAuthorizationBinding()` binds the selected issuer to the protected resource and requires S256 PKCE.
- `MergeAuthorizationScopes()` deduplicates and bounds requested scopes.

The OAuth flow uses state, PKCE S256, HTTPS endpoints, issuer checking, and the OAuth `resource` parameter. Public CIMD clients must not use a client secret. Token files are written atomically with mode `0600` and include issuer/resource binding fields. Runtime loading rejects credentials whose issuer or resource does not match the configured server.

The token file shape is:

```json
{
  "access_token": "ACCESS_TOKEN",
  "refresh_token": "REFRESH_TOKEN",
  "token_type": "Bearer",
  "scope": "repo read:user",
  "issuer": "https://auth.example.com",
  "resource": "https://example.com/mcp",
  "expires_at": 0
}
```

## Register MCP servers with std_slop

Unauthenticated server:

```sh
bazel run //app:std_slop -- mcp add local --url https://example.com/mcp --auth none
```

OAuth server with endpoint discovery:

```sh
bazel run //app:std_slop -- mcp add github \
  --url https://example.com/mcp \
  --auth oauth \
  --client-id CLIENT_ID
bazel run //app:std_slop -- mcp oauth-login github
```

`CLIENT_ID` must be a real client ID from a registered OAuth app or GitHub App. Discovery finds the authorization and token endpoints only; it cannot create or infer a client ID.

If that app also requires a client secret, pass it only when exchanging or refreshing tokens:

```sh
bazel run //app:std_slop -- mcp oauth-login github --client-secret CLIENT_SECRET
bazel run //app:std_slop -- mcp oauth-refresh github --client-secret CLIENT_SECRET
```

The client secret is not written to `mcp.ini` or the token file.

GitHub Copilot MCP example:

```sh
bazel run //app:std_slop -- mcp add githubcopilot \
  --url https://api.githubcopilot.com/mcp \
  --auth oauth \
  --client-id YOUR_REGISTERED_GITHUB_APP_CLIENT_ID \
  --scope read:user
bazel run //app:std_slop -- mcp oauth-login githubcopilot --client-secret YOUR_REGISTERED_GITHUB_APP_CLIENT_SECRET
```

If the server does not publish OAuth metadata, pass endpoints manually:

```sh
bazel run //app:std_slop -- mcp add github \
  --url https://example.com/mcp \
  --auth oauth \
  --client-id CLIENT_ID \
  --authorization-endpoint https://auth.example.com/authorize \
  --token-endpoint https://auth.example.com/token
```

Bearer-token server:

```sh
bazel run //app:std_slop -- mcp add private \
  --url https://example.com/mcp \
  --auth bearer \
  --token YOUR_TOKEN
```

`--token` is valid only with `--auth bearer`. The command writes the token to the per-server token file and does not write it to `mcp.ini`. Re-run `mcp add` with the same name to replace the saved token. By default, tokens are stored under `~/.config/slop/mcp/tokens/<name>.json`; pass `--token-path <path>` to use a different file.

The token file uses the shared token-store JSON shape. Bearer-only entries leave the binding fields empty:

```json
{
  "access_token": "YOUR_TOKEN",
  "refresh_token": "",
  "token_type": "Bearer",
  "scope": "",
  "issuer": "",
  "resource": "",
  "expires_at": 0
}
```

## Conformance and limits

| Area | Behavior |
| --- | --- |
| HTTP | HTTPS is required for OAuth metadata and token endpoints. HTTP responses and bodies are bounded. |
| Revision selection | Modern discovery is attempted first; under `kPreferLatest`, fallback occurs only for an HTTP 400 with no JSON-RPC error or an HTTP 200 JSON-RPC `-32601` method-not-found response. |
| Tool catalogs | Pagination is bounded, cursors must not cycle, and duplicate runtime names are rejected. |
| Tool calls | Input arguments and structured output are schema-validated. Modern `input_required` results can be resumed with `ContinueToolCall`. |
| Resources | Classic `resources/subscribe` and `resources/unsubscribe` are sent only when advertised; update notifications are queued. |
| OAuth | State, S256 PKCE, HTTPS, issuer/resource binding, scope limits, token rotation, and mode `0600` token files are enforced. |
| Failure recovery | The last successful catalog can be used during a transient refresh failure; database and executor replacement remains atomic. |

## Examples

Build the examples without contacting a live server:

```sh
bazel build //mcp:list_tools_example //mcp:call_tool_example
```

List tools from a server:

```sh
bazel run //mcp:list_tools_example -- https://example.com/mcp
```

Call a tool with JSON object arguments:

```sh
bazel run //mcp:call_tool_example -- https://example.com/mcp search '{"query":"mcp"}'
```

The examples create a real `HttpClient`, connect, initialize a session, and then run the requested MCP method. They require a live MCP Streamable HTTP endpoint at runtime.
