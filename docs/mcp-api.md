# MCP API and C++ client library

This document describes the reusable C++ Model Context Protocol (MCP) client library in this repository. It is for C++ callers that want to connect to Streamable HTTP MCP servers directly.

For the `std_slop mcp ...` command-line workflow, see [mcp-slop-userguide.md](mcp-slop-userguide.md).

## Scope

The library supports MCP Streamable HTTP servers. It does not implement stdio transport or the deprecated HTTP+SSE transport.

Core package:

```text
mcp/
  client.h                     high-level connection helper
  session.h                    typed MCP session API
  streamable_http_transport.h  HTTP transport implementation
  json_rpc.h                   JSON-RPC build/parse helpers
  types.h                      public MCP data types
  authorization.h              OAuth discovery metadata parsers
  oauth_discovery.h            OAuth endpoint discovery helper
  oauth_client.h               PKCE OAuth helpers
  token_store.h                token file persistence helpers
  registry.h                   server registry data model
```

The public API uses `absl::Status` and `absl::StatusOr<T>` for fallible operations.

## Protocol surface

The client speaks JSON-RPC 2.0 over Streamable HTTP and targets MCP protocol version `2025-11-25`.

Implemented lifecycle support:

- `initialize`
- initialize result validation
- `notifications/initialized`
- negotiated protocol version tracking
- server capability parsing

Implemented methods:

- `ping`
- `tools/list`
- `tools/call`
- `resources/list`
- `resources/read`
- `prompts/list`
- `prompts/get`

Implemented server notification parsing:

- progress notifications
- logging notifications
- tools/resources/prompts list-change notifications

Unsupported or host-policy dependent features:

- stdio transport
- deprecated HTTP+SSE transport
- automatic sampling approval
- automatic roots approval
- automatic elicitation approval
- full JSON Schema validation

## Connect to a Streamable HTTP server

Use `ConnectStreamableHttp()` for the common case. It creates the transport, sends `initialize`, validates the response, and returns an initialized `Session`.

```c++
#include "core/http_client.h"
#include "mcp/client.h"

slop::mcp::StreamableHttpConfig config;
config.endpoint_url = "https://example.com/mcp";

slop::mcp::InitializeOptions options;
options.client_info.name = "my-client";
options.client_info.version = "1.0";

slop::HttpClient http_client;
absl::StatusOr<std::unique_ptr<slop::mcp::Session>> session =
    slop::mcp::ConnectStreamableHttp(config, options, &http_client);
if (!session.ok()) return session.status();
```

After connection, use the typed session methods:

```c++
RETURN_IF_ERROR((*session)->Ping());

absl::StatusOr<std::vector<slop::mcp::Tool>> tools = (*session)->ListTools();
if (!tools.ok()) return tools.status();

absl::StatusOr<slop::mcp::ToolCallResult> result =
    (*session)->CallTool("search", nlohmann::json{{"query", "mcp"}});
if (!result.ok()) return result.status();
```

`ToolCallResult::is_error` represents an MCP tool-level error. It is not the same as a transport failure.

## Session API

`mcp/session.h` exposes:

```c++
absl::Status Close();
absl::Status Ping();
std::vector<ServerNotification> DrainNotifications();
absl::StatusOr<std::vector<Tool>> ListTools();
absl::StatusOr<ToolCallResult> CallTool(absl::string_view name, const nlohmann::json& arguments);
absl::StatusOr<std::vector<Resource>> ListResources();
absl::StatusOr<ResourceReadResult> ReadResource(absl::string_view uri);
absl::StatusOr<std::vector<Prompt>> ListPrompts();
absl::StatusOr<PromptGetResult> GetPrompt(absl::string_view name, const nlohmann::json& arguments);
```

Notifications received during requests are queued and returned by `DrainNotifications()`.

## Transport behavior

`StreamableHttpTransport` sends JSON-RPC messages to one MCP endpoint.

Request headers include:

- `Content-Type: application/json`
- `Accept: application/json, text/event-stream`
- `MCP-Protocol-Version: <version>` after initialization
- `Mcp-Session-Id: <id>` when the server provides one
- `Authorization: Bearer <token>` when `bearer_token` is configured
- any caller-supplied `extra_headers`

Responses can be direct JSON or `text/event-stream`. SSE event `data:` payloads are parsed as JSON-RPC messages.

## Bearer token clients

For static bearer authentication, put the token into `StreamableHttpConfig::bearer_token` before connection:

```c++
slop::mcp::StreamableHttpConfig config;
config.endpoint_url = "https://example.com/mcp";
config.bearer_token = "YOUR_TOKEN";
```

The transport injects:

```text
Authorization: Bearer YOUR_TOKEN
```

Do not log token values. If you persist tokens with `SaveOAuthTokens()`, the token store rejects access tokens with HTTP header control characters so they cannot create malformed Authorization headers.

## OAuth clients

The library has three OAuth pieces:

1. Endpoint discovery in `mcp/oauth_discovery.h`.
2. Discovery parsers in `mcp/authorization.h`.
3. PKCE authorization-code helpers in `mcp/oauth_client.h`.

### OAuth endpoint discovery

Use `DiscoverOAuthEndpoints()` when a host wants the library to do the full MCP OAuth discovery sequence from an MCP endpoint URL:

```c++
#include "mcp/oauth_discovery.h"

absl::StatusOr<slop::mcp::OAuthDiscoveryResult> discovery =
    slop::mcp::DiscoverOAuthEndpoints(&http_client, "https://example.com/mcp");
if (!discovery.ok()) return discovery.status();

slop::mcp::OAuthClientConfig oauth;
oauth.authorization_endpoint = discovery->authorization_endpoint;
oauth.token_endpoint = discovery->token_endpoint;
oauth.scopes = discovery->scopes_supported;
```

`OAuthDiscoveryResult` contains:

```c++
struct OAuthDiscoveryResult {
  std::string resource_metadata_url;
  std::string authorization_server_url;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::vector<std::string> scopes_supported;
};
```

The discovery helper is part of the reusable MCP library API. It performs the MCP protected-resource and authorization-server discovery flow:

1. POSTs an unauthenticated MCP `initialize` request to the MCP endpoint.
2. Expects `401 Unauthorized` with a Bearer `WWW-Authenticate` header.
3. Extracts the `resource_metadata` URL.
4. Fetches and validates protected resource metadata.
5. Reads the single advertised `authorization_servers` entry.
6. Builds the OAuth authorization-server metadata URL from that issuer:
   - If the value already contains `/.well-known/`, it is used as-is.
   - Otherwise the helper inserts `/.well-known/oauth-authorization-server` after the origin and before any issuer path.
7. Fetches and validates authorization server metadata.
8. Returns the authorization endpoint, token endpoint, protected resource metadata URL, authorization server URL, and supported scopes.

Discovery requires HTTPS for the resource metadata URL, authorization server URL, authorization endpoint, and token endpoint. It also requires exactly one advertised authorization server. If a server advertises zero or multiple authorization servers, the host should ask the user to pass endpoints manually.

The caller still supplies a registered OAuth `client_id`. Discovery cannot create or infer a client ID.

### OAuth discovery metadata

Use `ParseWwwAuthenticateResourceMetadata()` to extract the protected resource metadata URL from a Bearer `WWW-Authenticate` header:

```c++
absl::StatusOr<std::string> metadata_url =
    slop::mcp::ParseWwwAuthenticateResourceMetadata(www_authenticate_header);
```

Then fetch and parse metadata:

```c++
absl::StatusOr<slop::mcp::ProtectedResourceMetadata> resource =
    slop::mcp::ParseProtectedResourceMetadata(resource_json);

absl::StatusOr<slop::mcp::AuthorizationServerMetadata> auth_server =
    slop::mcp::ParseAuthorizationServerMetadata(auth_server_json);
```

The endpoints are not guessed from the MCP endpoint URL. They come from two metadata documents:

1. The `WWW-Authenticate` challenge points to protected resource metadata:

   ```http
   WWW-Authenticate: Bearer resource_metadata="https://api.example.com/.well-known/oauth-protected-resource/mcp"
   ```

2. Protected resource metadata names the authorization server issuer:

   ```json
   {
     "resource": "https://api.example.com/mcp",
     "authorization_servers": ["https://auth.example.com"]
   }
   ```

3. The library derives the authorization-server metadata URL from that issuer. For `https://auth.example.com`, it fetches:

   ```text
   https://auth.example.com/.well-known/oauth-authorization-server
   ```

   For an issuer with a path, such as `https://auth.example.com/tenant-a`, it fetches:

   ```text
   https://auth.example.com/.well-known/oauth-authorization-server/tenant-a
   ```

4. Authorization-server metadata provides the concrete endpoints used by PKCE:

   ```json
   {
     "issuer": "https://auth.example.com",
     "authorization_endpoint": "https://auth.example.com/authorize",
     "token_endpoint": "https://auth.example.com/token"
   }
   ```

`ParseAuthorizationServerMetadata()` reads those `authorization_endpoint` and `token_endpoint` values. `DiscoverOAuthEndpoints()` performs all four steps and returns both endpoints in `OAuthDiscoveryResult`.

The generic MCP client keeps MCP-specific discovery parsing in `mcp/authorization.*`. It does not put provider-specific OAuth policy into the transport.

### PKCE browser flow

Use `OAuthClientConfig` and `StartPkceAuthorization()` to start an authorization-code + PKCE flow:

```c++
slop::mcp::OAuthClientConfig oauth;
oauth.client_id = "CLIENT_ID";
oauth.authorization_endpoint = "https://auth.example.com/authorize";
oauth.token_endpoint = "https://auth.example.com/token";
oauth.scopes = {"repo"};

auto authorization = slop::mcp::StartPkceAuthorization(oauth);
if (!authorization.ok()) return authorization.status();

// Show authorization->authorization_url to the user, receive callback URL,
// then extract and exchange the code.
auto code = slop::mcp::ExtractAuthorizationCodeFromCallback(callback_url, authorization->state);
if (!code.ok()) return code.status();

auto tokens = slop::mcp::ExchangeAuthorizationCode(&http_client, oauth, *code, authorization->code_verifier);
if (!tokens.ok()) return tokens.status();
```

Confidential OAuth clients can set `oauth.client_secret` for token exchange and refresh. Do not persist client secrets unless your host application has an explicit secret-storage policy.

Refresh a token with:

```c++
auto refreshed = slop::mcp::RefreshOAuthToken(&http_client, oauth, old_refresh_token);
```

Use the returned `OAuthTokenSet::access_token` as `StreamableHttpConfig::bearer_token`.

## Token persistence

`mcp/token_store.h` provides:

```c++
absl::Status SaveOAuthTokens(const std::string& path, const OAuthTokenSet& tokens);
absl::StatusOr<OAuthTokenSet> LoadOAuthTokens(const std::string& path);
absl::Status DeleteOAuthTokens(const std::string& path);
```

Token JSON shape:

```json
{
  "access_token": "TOKEN",
  "refresh_token": "REFRESH_TOKEN",
  "expires_at": 0
}
```

`SaveOAuthTokens()` creates parent directories as needed and writes token files with restricted permissions.

## Error model

Use status codes to distinguish failures:

- malformed JSON-RPC or response shape: `InvalidArgument`
- unsupported MCP protocol version: `Unimplemented`
- HTTP 401: `Unauthenticated`
- HTTP 403: `PermissionDenied`
- other HTTP failure: `Unavailable`
- no queued response available: `Unavailable`
- closed or uninitialized session: `FailedPrecondition`

Callers should add host-specific remediation text at their boundary. For example, `std::slop` adds messages that tell users to run `std_slop mcp add ... --auth bearer --token ...`, `std_slop mcp login`, or `std_slop mcp refresh`.

## Security notes

- Treat all server messages as untrusted.
- Validate JSON-RPC shape before session side effects.
- Validate tool result content before converting it to host output.
- Do not execute server-provided data.
- Do not log bearer tokens or Authorization headers.
- Do not fetch server-provided icons with credentials unless the host policy allows it.
- Require explicit host callbacks for sampling, roots, and elicitation.
- Roots are not a security boundary; use OS sandboxing or file permissions for enforcement.
