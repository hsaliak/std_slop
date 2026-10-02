# Coding-agent walkthrough

This guide covers the `std_slop` terminal UI and the shared agent setup used by `sl`. For Markdown and MCP library entry points, use the [documentation index](README.md). For scripted prompts and state commands, see the [sl CLI guide](sl.md).

## Build

```bash
git clone https://github.com/hsaliak/std_slop.git
cd std_slop
bazel test //...
bazel build //...
mkdir -p "$HOME/bin"
cp bazel-bin/app/std_slop bazel-bin/app/sl "$HOME/bin/"
```

Prebuilt macOS and Linux x86-64 binaries are available from the [releases page](https://github.com/hsaliak/std_slop/releases).

## Authentication

Choose one path:

1. **API key:** set `OPENAI_API_KEY`. Set `openai_base_url` for an OpenAI-compatible endpoint.
2. **OpenAI OAuth:** run `std_slop --fetch_openai_oauth_token` or `std_slop --fetch_openai_oauth_device_token`, then start with `std_slop --openai_oauth`.

For configuration examples, copy `docs/example_config.ini` to `~/.config/slop/config.ini`:

```bash
mkdir -p ~/.config/slop
cp docs/example_config.ini ~/.config/slop/config.ini
std_slop
```

CLI settings override INI settings, which override environment variables, which override defaults. See [OAUTH.md](OAUTH.md) for OAuth details.

## First Run

Run one prompt:

```bash
std_slop --prompt "Summarize the repository structure"
```

For a scripted run with a persistent database:

```bash
sl --prompt "Summarize the repository structure"
```

Or launch the interactive UI:

```bash
std_slop
```

Useful commands:

```text
/models
/help
/session switch scratch
```

## Subquery Tools

INI sections named `[llm_tool_<name>]` register specialized `llm_query` tools. See [example_subqueries.ini](example_subqueries.ini) and [impl/subqueries.md](impl/subqueries.md).

## Next Steps

- [README.md](../README.md): monorepo components and agent batch mode.
- [sl.md](sl.md): scripted prompts, JSON output, and state commands.
- [OAUTH.md](OAUTH.md): OAuth details.
- [mail_mode.md](mail_mode.md): patch-based workflow.
- [README.md](README.md): documentation index.
