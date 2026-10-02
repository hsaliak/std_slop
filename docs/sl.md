# sl command-line guide

`sl` is the non-interactive coding-agent interface in the `std::slop` monorepo. Use it for prompts in scripts and for commands that inspect or change agent state. Use `std_slop` for the interactive terminal UI.

Both interfaces use the shared agent runtime, model configuration, and tools. See the [agent walkthrough](WALKTHROUGH.md) for authentication and `config.ini` setup. The [monorepo README](../README.md) also covers `std_slop` batch mode.

## Build

```sh
bazel build //:sl
```

The binary is `bazel-bin/app/sl`. Add it to your PATH if needed:

```sh
mkdir -p "$HOME/bin"
cp bazel-bin/app/sl "$HOME/bin/sl"
```

## Prompts and input

Use one instruction source: `--prompt` or `--prompt_file`. Piped stdin is optional context, not a replacement for the instruction source.

```sh
sl --prompt "Review the authentication flow"
sl --prompt_file task.md --session incident
cat build.log | sl --prompt "Explain this failure"
sl --db /tmp/project.db --prompt "Inspect the database"
sl --ephemeral --prompt "Try this without changing the ledger"
```

Live prompts need model configuration and credentials. Use the same setup described in the walkthrough.

## Database selection

Without `--db` or `--ephemeral`, `sl` uses the configured database or `slop.db`. `--db` and `--ephemeral` are mutually exclusive.

| Interface | Default database | Override |
| --- | --- | --- |
| Interactive `std_slop` | Configured database or `slop.db` | Agent configuration and flags. |
| `std_slop` batch mode | In-memory database | `--prompt_db`. |
| `sl` | Configured database or `slop.db` | `--db` for a path; `--ephemeral` for an in-memory run. |

Keep the database and SQLite sidecar files outside the repository, or ignore them in Git. The ledger can contain sensitive prompt and tool data.

## JSON and schema output

Every JSON-producing command must explicitly use `--json`. `--schema` requires `--json` and emits the validated structured value directly.

```sh
sl --prompt "Extract the issue" --json
sl --prompt "Extract the issue" --json --schema issue.schema.json
sl context show --json
sl session list --json
```

The flag names differ from `std_slop` batch mode:

| Output | `sl` | `std_slop` batch mode |
| --- | --- | --- |
| Run metadata | `--json` | `--output=json` |
| Schema-constrained value | `--json --schema schema.json` | `--format_file schema.json` or inline `--format` |

For `std_slop` batch output fields and the supported schema subset, see [interactive and batch usage](../README.md#interactive-and-batch-usage). Structured batch output cannot be combined with `--output=json`.

## State subcommands

Database and agent state are available through shell-friendly subcommands. Slash commands are available only in the interactive `std_slop` TUI.

```sh
sl context show
sl context set --retain_groups=2 --watermark_tokens=350000
sl session list
sl message list
sl scratchpad show
sl skill list
sl stats
sl tool list
sl mcp list
```

These examples cover context, sessions, messages, scratchpad state, skills, statistics, tools, and registered MCP endpoints. The outbound MCP client supports HTTP endpoints; `sl mcp` does not launch the repository's stdio echo server.

## MCP commands and configuration

`sl mcp` uses the same HTTP server registry and command handler as `std_slop mcp`. These commands manage registrations and token files, not the agent session database.

| Command | Purpose |
| --- | --- |
| `sl mcp add NAME --url URL [options]` | Add a server, or replace the entry with the same name. |
| `sl mcp list` | List saved servers, auth modes, enabled state, and URLs. |
| `sl mcp oauth-login NAME [--client-secret SECRET]` | Run browser-paste OAuth login; see the prompt limitation below. |
| `sl mcp oauth-refresh NAME [--client-secret SECRET]` | Refresh a saved OAuth token. |
| `sl mcp logout NAME` | Delete the token but keep the registration. |
| `sl mcp remove NAME` | Remove the registration and attempt to delete its token. |
| `sl mcp help` | Show the shared MCP command help. |

The command names are `oauth-login` and `oauth-refresh`, not `login` and `refresh`. Options use separate arguments, such as `--auth bearer`, not `--auth=bearer`.

### Register a server

```sh
# No authentication (--auth none is the default).
sl mcp add public --url https://example.com/mcp

# Static bearer token; do not paste secrets into shared shell history.
sl mcp add private --url https://example.com/mcp \
  --auth bearer --token "$TOKEN"

# OAuth endpoint discovery; the provider must supply a registered client ID.
sl mcp add work --url https://example.com/mcp \
  --auth oauth --client-id "$CLIENT_ID" --scope read --scope write
```

`--auth` accepts `none`, `bearer`, or `oauth`. Bearer mode requires `--token`; OAuth mode requires `--client-id`. `--scope` may be repeated. `--token-path PATH` overrides the default token location. Server names use letters, digits, hyphens, or underscores.

If OAuth discovery is unavailable, supply both HTTPS endpoints and the issuer:

```sh
sl mcp add work --url https://example.com/mcp \
  --auth oauth --client-id "$CLIENT_ID" \
  --authorization-endpoint https://auth.example.com/authorize \
  --token-endpoint https://auth.example.com/token \
  --issuer https://auth.example.com
```

### Files and output

- Registrations are stored in `~/.config/slop/mcp.ini`, in `[server.NAME]` sections. Repeating `add` replaces an entry; omitted fields are not merged with the old entry.
- Tokens default to `~/.config/slop/mcp/tokens/NAME.json` and are saved with mode `0600`. Tokens are not stored in the registry or SQLite. Client secrets are not saved.
- MCP management uses the default registry path. Agent flags such as `--config`, `--db`, `--ephemeral`, and `--session` do not select a different MCP registry or make registration changes temporary.
- `sl mcp list --json` returns the command's text under `result.output`, not a structured array of servers.
- To disable a server without removing it, set `enabled = false` in its registry section. There is no enable/disable CLI command.

For example:

```sh
sl mcp list --json
sl mcp oauth-refresh work
sl mcp logout work
sl mcp remove work
```

OAuth login needs interactive input. `sl` currently buffers the shared handler's output, so its URL and prompt are not displayed before the callback read. Use `std_slop mcp oauth-login work` for browser-paste login. Refresh can use `sl`; pass `--client-secret` if the provider requires it.

For registry examples, discovery requirements, and the full login flow, see the [MCP integration guide](mcp-slop-userguide.md). These commands cannot register or launch a stdio server.

## Related guides

- [Sessions](SESSIONS.md): session state and ownership.
- [Context management](CONTEXT_MANAGEMENT.md): retained history and token watermarks.
- [Personas and skills](CONTEXT.md): shared instructions and modular skills.
- [Mail workflows](mail_mode.md): staging, review, approval, and finalization protections.
- [MCP agent integration](mcp-slop-userguide.md): external HTTP tools and authorization.
- [Agent database and schema](SCHEMA.md): the runtime data model.
