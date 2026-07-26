# MCP client

This package provides a small Model Context Protocol (MCP) client for Streamable HTTP servers.

## Connect to a Streamable HTTP server

Use `ConnectStreamableHttp` with an endpoint URL and initialization options:

```c++
slop::mcp::StreamableHttpConfig config;
config.endpoint_url = "https://example.com/mcp";

slop::mcp::InitializeOptions options;
options.client_info.name = "my-client";
options.client_info.version = "1.0";

slop::HttpClient http_client;
auto session = slop::mcp::ConnectStreamableHttp(config, options, &http_client);
```

After initialization, the `Session` supports:

- `Ping()`
- `ListTools()` and `CallTool()`
- `ListResources()` and `ReadResource()`
- `ListPrompts()` and `GetPrompt()`

## Authorization helpers

`authorization.h` contains helpers for OAuth discovery used by MCP protected resources:

- `ParseWwwAuthenticateResourceMetadata()` extracts the `resource_metadata` URL from a Bearer `WWW-Authenticate` header.
- `ParseProtectedResourceMetadata()` validates protected resource metadata JSON.
- `ParseAuthorizationServerMetadata()` validates authorization server metadata JSON.
- `TokenProvider` is a narrow interface for callers that manage access tokens outside the MCP session.

The helpers parse metadata used by OAuth discovery. The `std_slop mcp add --auth oauth` command can use this metadata to discover authorization and token endpoints, while `std_slop mcp login` performs the PKCE browser-paste flow and stores credentials in the per-server token file. Confidential OAuth clients can pass `--client-secret <secret>` to `mcp login` and `mcp refresh`; the secret is used for that request and is not stored.

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
bazel run //app:std_slop -- mcp login github
```

`CLIENT_ID` must be a real client ID from a registered OAuth app or GitHub App. Discovery finds the authorization and token endpoints only; it cannot create or infer a client ID.

If that app also requires a client secret, pass it only when exchanging or refreshing tokens:

```sh
bazel run //app:std_slop -- mcp login github --client-secret CLIENT_SECRET
bazel run //app:std_slop -- mcp refresh github --client-secret CLIENT_SECRET
```

The client secret is not written to `mcp.ini` or the token file.

GitHub Copilot MCP example:

```sh
bazel run //app:std_slop -- mcp add githubcopilot \
  --url https://api.githubcopilot.com/mcp \
  --auth oauth \
  --client-id YOUR_REGISTERED_GITHUB_APP_CLIENT_ID \
  --scope read:user
bazel run //app:std_slop -- mcp login githubcopilot --client-secret YOUR_REGISTERED_GITHUB_APP_CLIENT_SECRET
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
