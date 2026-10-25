# std::slop MCP user guide

This guide explains how the shared agent runtime uses Model Context Protocol (MCP) servers. `std_slop mcp` and `sl mcp` use the same registrations and token files; see the [sl guide](sl.md) for its output and routing rules.

For the reusable C++ MCP library API, see [mcp-api.md](mcp-api.md).

This guide covers outbound HTTP MCP servers and local stdio MCP subprocesses. The repository also has a separate inbound stdio server API and echo example; see [mcp-server.md](mcp-server.md).

## What MCP adds to std_slop

`std_slop` reads the shared registry at application startup, connects to configured HTTP servers, starts enabled stdio subprocesses, discovers their tools, and projects those tools into the normal tool catalog. HTTP supports classic MCP `2025-11-25` and modern MCP `2026-07-28`; stdio supports modern `2026-07-28` only.

Runtime behavior:

1. `std_slop` reads the MCP registry from `~/.config/slop/mcp.ini`.
2. For each enabled HTTP server, it prefers modern MCP and falls back to classic only when modern discovery shows it is unsupported. For each enabled stdio server, it starts the configured executable directly and requires modern MCP; it never falls back to classic.
3. Tools are discovered with `tools/list`.
4. Discovered tools are registered as top-level tools named `mcp_<server>_<tool>`.
5. Calls route back to the matching HTTP server or live stdio process.
6. Stale `mcp_` tool rows are removed at startup before discovery.
7. Stdio children stay alive for the agent runtime and are stopped when it ends. `sl` starts them for each prompt invocation; `std_slop` keeps them for its session.

If a server cannot be started or authenticated, its tools are not exposed to the model.

## Command overview

```sh
std_slop mcp <command> [arguments]
```

Commands:

```text
add <name> --url <mcp_endpoint> [--auth none|bearer|oauth] [--token <token>] [--token-path <path>] [--client-id <id>] [--scope <scope>...]
add <name> --transport stdio --command <executable> [--args-json '<string-array>']
list
oauth-login <name>
oauth-refresh <name>
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
transport = http
url = https://api.githubcopilot.com/mcp
auth = bearer
enabled = true
token_path = /Users/example/.config/slop/mcp/tokens/github.json
```

Tokens are not written to `mcp.ini`. They are saved in the token file.

Token JSON (selected fields):

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
local       http    none    enabled https://example.com/mcp
local-files stdio   none    enabled /absolute/path/to/mcp-server
```

## Local stdio servers

Register a trusted local executable with a JSON array of arguments:

```sh
std_slop mcp add local-files \
  --transport stdio \
  --command /absolute/path/to/mcp-server \
  --args-json '["--root", "/work/project"]'
```

`--args-json` is optional; if omitted, the server receives no extra arguments. If provided, it must be a JSON array of strings. The host passes `command` and each array item as literal argv values; it does not invoke a shell or expand `$VAR`/`~`. The executable may be an absolute path or a program found on `PATH`.

The registry form stores the argument array in `args_json`; if the field is omitted, it defaults to `[]`.

The registry form is:

```ini
[server.local-files]
transport = stdio
command = /absolute/path/to/mcp-server
args_json = ["--root", "/work/project"]
enabled = true
```

`mcp add` and `mcp list` do not start the process. The runtime starts each enabled entry at startup and keeps it for that runtime's lifetime. Stdio servers must support modern MCP `2026-07-28`; classic stdio, OAuth, and bearer authentication are not supported. Child stderr goes to the agent's stderr, while stdout is reserved for MCP messages.

A stdio child runs as the same user as the agent and inherits its environment and working directory. It can access the same local files and environment variables. Register only programs you trust. `mcp list` displays the transport and command, but does not display argv values.

### Bundled local workspace tools

Build and register the local workspace service:

```sh
bazel build //mcp/local_tools:local_tools_server
std_slop mcp add local-tools \
  --transport stdio \
  --command /absolute/path/to/bazel-bin/mcp/local_tools/local_tools_server
```

The service needs no extra arguments or saved workspace path. It captures its launch directory as the default workspace, `launch`, then discovers linked Git worktrees when it lists or resolves workspaces. `workspace_list` refreshes the catalog on every call; `workspace_info` returns one workspace root. A workspace ID is `launch` or `worktree:/absolute/path`. Omit `workspace` to use `launch`.

#### Tool reference

Each top-level tool argument object rejects unknown fields. File paths are relative to the selected workspace; absolute paths, `..` traversal, and symlink traversal are rejected. The bundled stdio server also enforces its default 1 MiB input-line limit, including the JSON-RPC envelope. The file service has no dedicated Git MCP tools.

| Tool | Arguments and behavior |
| --- | --- |
| `workspace_list` | No arguments. Lists the launch workspace and currently linked Git worktrees. |
| `workspace_info` | Optional `workspace`; defaults to `launch`. Returns the selected root path. |
| `read_file` | Required `path`; optional `workspace`, `start_line`, `end_line` (1-based), and `line_numbers` (default `false`). Reads at most 4 MiB. |
| `list_directory` | Optional `path` (default `.`), `workspace`, `depth` (default 1, range 1–8), `include_ignored` (default `false`), and `ignore` (additional patterns). Lists at most 1,000 entries. By default it skips `.git`, `node_modules`, `bazel-*`, `dist`, `build`, `.cache`, `.next`, and `target`; `include_ignored` disables those defaults. |
| `grep` | Required `pattern`; optional `path` (default `.`), `workspace`, `context` (default 0, range 0–20), `limit` (default 200, range 1–1,000), `fixed_strings` (default `false`), `include_ignored` (default `false`), and `ignore` (additional patterns). Patterns are limited to 4 KiB; a search scans at most 10,000 files and 32 MiB of file content, and returned output is limited to 1 MiB. `limit` caps returned matches. RE2 regular-expression matching is the default; set `fixed_strings` to `true` for literal matching. |
| `write_file` | Required `path` and `content`; optional `workspace`. Writes atomically; the underlying file operation accepts up to 4 MiB, subject to the server's 1 MiB JSON input-line limit. |
| `edit_tool` | Required `path` and non-empty `edits` array; optional `workspace`. Supports `replace`, `insert_before`, `insert_after`, and `delete`; at most 256 edits. Each edit uses exact, non-empty `find` text. Supply non-empty `text` for replace/insert operations. `which` selects `only` (default), `first`, `last`, or a zero-based match index. |
| `execute_bash` | Required non-empty `command` (maximum 64 KiB); optional `workspace`, workspace-relative `cwd` (default `.`), `timeout_seconds` (default 30, range 1–45), and `allow_nonzero_exit` (default `false`). Combined stdout/stderr is limited to 1 MiB. |

For example, this edit replaces the first exact match in a selected worktree:

```json
{
  "workspace": "worktree:/work/project-feature",
  "path": "src/config.h",
  "edits": [{"op": "replace", "find": "old_value", "text": "new_value", "which": "first"}]
}
```

`execute_bash` starts in the selected workspace, or its optional workspace-relative `cwd`, but it is **not a sandbox**. A command can use absolute paths, change directory, run Git, or access files outside the workspace. Register and use this server only when you trust the process and command source.

In mail mode, every dynamically imported MCP tool requires the host to be on a `slop/staging/*` branch, including tools that declare a read-only hint. Native read tools and the staging-branch bootstrap tool remain available off staging. This is a branch gate, not sandboxing: a call allowed on staging can still affect another worktree or external system.

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

If the resource requires particular permissions, select its scopes with one or more `--scope` options when you add the server. Do not assume a successful OAuth login grants every tool permission.

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

OAuth scopes are provider-specific. `--scope` may be repeated; request every scope required by the tools you plan to use. `std_slop` does not infer the required scope set. A successful browser login with missing scopes can still leave calls denied. After changing scopes, run `oauth-login` again to obtain a token with the new grants.

SondaHub's public test endpoint needs no OAuth. For its protected endpoint, `mcp:read` grants read tools and `mcp:write` is needed for write tools:

```sh
# Public HTTP endpoint.
std_slop mcp add sondahub-public --url https://api.sondahub.com/mcp

# OAuth endpoint with read and write access.
std_slop mcp add sondahub \
  --url https://api.sondahub.com/mcp/secure \
  --auth oauth --client-id sonda \
  --scope mcp:read --scope mcp:write
std_slop mcp oauth-login sondahub
```

What `std_slop` does during discovery:

1. Sends an unauthenticated MCP `initialize` request to the MCP endpoint.
2. Reads the Bearer `WWW-Authenticate` header from the `401 Unauthorized` response.
3. Extracts the `resource_metadata` URL.
4. Fetches protected resource metadata.
5. Reads the single advertised authorization server URL from `authorization_servers`.
6. Derives the authorization-server metadata URL. If the advertised URL does not already contain `/.well-known/`, `std_slop` inserts `/.well-known/oauth-authorization-server` after the origin and before any issuer path.
7. Fetches authorization-server metadata.
8. Stores `authorization_endpoint`, `token_endpoint`, `resource_metadata_url`, and `authorization_server_url` in `mcp.ini`.

Discovery requires HTTPS metadata and endpoint URLs, and it requires exactly one advertised authorization server. If discovery fails, pass both endpoints and the issuer manually.

### Register with manual endpoints

If discovery is not available, pass both HTTPS endpoints and the issuer:

```sh
std_slop mcp add github \
  --url https://example.com/mcp \
  --auth oauth \
  --client-id CLIENT_ID \
  --authorization-endpoint https://auth.example.com/authorize \
  --token-endpoint https://auth.example.com/token \
  --issuer https://auth.example.com
```

Do not pass only one endpoint. Manual configuration requires both endpoints and `--issuer`.

### OAuth login

Start browser-paste login:

```sh
std_slop mcp oauth-login github
```

The command prints an authorization URL. Open it in a browser, complete the provider flow, then paste the callback URL at the prompt.

If the OAuth client requires a secret, enter it into `SLOP_MCP_CLIENT_SECRET` through a secure prompt before login or refresh:

```sh
read -rs -p 'OAuth client secret: ' SLOP_MCP_CLIENT_SECRET
printf '\n'
export SLOP_MCP_CLIENT_SECRET
std_slop mcp oauth-login github
```

The secret is not accepted as a command-line argument and is not stored in the registry or token file.

### OAuth refresh

Refresh a stored OAuth token:

```sh
std_slop mcp oauth-refresh github
```

If the OAuth client requires a secret for refresh, keep `SLOP_MCP_CLIENT_SECRET` set and run:

```sh
std_slop mcp oauth-refresh github
unset SLOP_MCP_CLIENT_SECRET
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
std_slop mcp oauth-login <name>
```

or:

```sh
std_slop mcp oauth-refresh <name>
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
