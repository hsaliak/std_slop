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

## Related guides

- [Sessions](SESSIONS.md): session state and ownership.
- [Context management](CONTEXT_MANAGEMENT.md): retained history and token watermarks.
- [Personas and skills](CONTEXT.md): shared instructions and modular skills.
- [Mail workflows](mail_mode.md): staging, review, approval, and finalization protections.
- [MCP agent integration](mcp-slop-userguide.md): external HTTP tools and authorization.
- [Agent database and schema](SCHEMA.md): the runtime data model.
