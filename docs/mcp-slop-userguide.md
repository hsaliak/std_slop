# std::slop MCP user guide

This guide explains how `std_slop` uses Model Context Protocol (MCP) servers.

For the reusable C++ MCP library API, see [mcp-api.md](mcp-api.md).

## What MCP adds to std_slop

`std_slop` can start configured Streamable HTTP MCP servers at application startup, discover their tools, and project those tools into the normal tool catalog.

Runtime behavior:

1. `std_slop` reads the MCP registry from `~/.config/slop/mcp.ini`.
2. Each enabled server is started.
3. Tools are discovered with `tools/list`.
4. Discovered tools are registered as top-level tools named:

   ```text
   mcp_<server>_<tool>
   ```

5. Calls to those tools route back to the matching MCP server.
6. Stale `mcp_` tool rows are removed at startup before discovery.

If a server cannot be started or authenticated, its tools are not exposed to the model.

## Command overview

```sh
std_slop mcp <command> [arguments]
```

Commands:

```text
add <name> --url <mcp_endpoint> [--auth none|bearer|oauth] [--token <token>] [--token-path <path>] [--client-id <id>] [--scope <scope>...]
list
login <name> [--client-secret <secret>]
refresh <name> [--client-secret <secret>]
logout <name>
remove <name>
help
```

Server names can contain letters, digits, hyphens, and underscores. The same name is used in runtime tool names, so keep it short and descriptive.

## Registry and token files

Default registry path:

```text
~/.config/slop/mcp.ini
```

Default token path:

```text
~/.config/slop/mcp/tokens/<name>.json
```

A registry entry looks like:

```ini
[server.github]
url = https://api.githubcopilot.com/mcp
auth = bearer
enabled = true
token_path = /Users/example/.config/slop/mcp/tokens/github.json
```

Tokens are not written to `mcp.ini`. They are saved in the token file.

Token JSON shape:

```json
{
  "access_token": "TOKEN",
  "refresh_token": "",
  "expires_at": 0
}
```

## Unauthenticated servers

Use `--auth none` for servers that do not require credentials:

```sh
std_slop mcp add local --url https://example.com/mcp --auth none
```

List configured servers:

```sh
std_slop mcp list
```

Example output:

```text
local   none    enabled https://example.com/mcp
```

## Bearer-token servers

Use `--auth bearer --token <token>` for a static bearer token:

```sh
std_slop mcp add github \
  --url https://api.githubcopilot.com/mcp \
  --auth bearer \
  --token YOUR_TOKEN
```

Rules:

- `--token` is valid only with `--auth bearer`.
- `--auth bearer` requires `--token`.
- The token is saved to the per-server token file.
- The token is not saved to `mcp.ini`.
- Re-run `mcp add` with the same name to replace the saved token.
- Pass `--token-path <path>` to use a non-default token file.

Example with a custom token path:

```sh
std_slop mcp add github \
  --url https://api.githubcopilot.com/mcp \
  --auth bearer \
  --token YOUR_TOKEN \
  --token-path ~/.config/slop/mcp/tokens/github.json
```

If a bearer token is missing or invalid at startup, `std_slop` logs a warning and does not expose that server's tools.

## OAuth servers

Use `--auth oauth` for servers that support OAuth authorization-code + PKCE.

### Register with discovery

If the server publishes protected resource metadata, omit explicit endpoints and let `std_slop` discover them:

```sh
std_slop mcp add github \
  --url https://example.com/mcp \
  --auth oauth \
  --client-id CLIENT_ID \
  --scope repo
```

`CLIENT_ID` must come from a registered OAuth or GitHub App. Discovery finds endpoints; it does not create or infer a client ID.

What `std_slop` does during discovery:

1. Sends an unauthenticated MCP `initialize` request to the MCP endpoint.
2. Reads the Bearer `WWW-Authenticate` header from the `401 Unauthorized` response.
3. Extracts the `resource_metadata` URL.
4. Fetches protected resource metadata.
5. Reads the single advertised authorization server URL from `authorization_servers`.
6. Derives the authorization-server metadata URL. If the advertised URL does not already contain `/.well-known/`, `std_slop` inserts `/.well-known/oauth-authorization-server` after the origin and before any issuer path.
7. Fetches authorization-server metadata.
8. Stores `authorization_endpoint`, `token_endpoint`, `resource_metadata_url`, and `authorization_server_url` in `mcp.ini`.

Discovery requires HTTPS metadata and endpoint URLs, and it requires exactly one advertised authorization server. If discovery fails, pass both endpoints manually.

### Register with manual endpoints

If discovery is not available, pass both endpoints:

```sh
std_slop mcp add github \
  --url https://example.com/mcp \
  --auth oauth \
  --client-id CLIENT_ID \
  --authorization-endpoint https://auth.example.com/authorize \
  --token-endpoint https://auth.example.com/token
```

Do not pass only one endpoint. Manual fallback requires both.

### Login

Start browser-paste login:

```sh
std_slop mcp login github
```

The command prints an authorization URL. Open it in a browser, complete the provider flow, then paste the callback URL at the prompt.

If the OAuth client requires a secret, pass it only when exchanging the code:

```sh
std_slop mcp login github --client-secret CLIENT_SECRET
```

The client secret is used for that request and is not stored.

### Refresh

Refresh a stored OAuth token:

```sh
std_slop mcp refresh github
```

If the OAuth client requires a secret for refresh:

```sh
std_slop mcp refresh github --client-secret CLIENT_SECRET
```

### Logout

Delete the stored token file:

```sh
std_slop mcp logout github
```

## Remove a server

Remove a registry entry and its token file:

```sh
std_slop mcp remove github
```

## Runtime tool names

MCP tools are exposed with this shape:

```text
mcp_<server>_<tool>
```

Examples:

```text
mcp_github_search_repositories
mcp_github_create_pull_request
```

Names are sanitized for provider tool-name limits. If two discovered tools collide after sanitization, startup rejects that server's projected tools.

## Diagnostics

Common messages and fixes:

### Bearer token missing or invalid

Fix:

```sh
std_slop mcp add <name> --url <mcp_endpoint> --auth bearer --token <token>
```

### OAuth token missing or invalid

Fix:

```sh
std_slop mcp login <name>
```

or:

```sh
std_slop mcp refresh <name>
```

### Permission denied

For bearer servers, check token scopes or provider permissions.

For OAuth servers, check app scopes and then refresh or login again.

## Security notes

- Do not paste bearer tokens into shared terminals or logs.
- `std_slop` does not print token values in normal command output.
- Bearer tokens are stored in the token file, not in `mcp.ini`.
- Client secrets for OAuth login and refresh are not stored.
- MCP server content is untrusted. `std_slop` validates protocol and tool-result shapes before exposing results.
