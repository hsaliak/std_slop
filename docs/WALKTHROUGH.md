# Walkthrough

This walkthrough is the fastest way to get `std::slop` working when you are starting from a fresh machine or an empty `~/.config/slop` directory.

**Audience:** first-time and returning users who want the canonical setup flow.
**Related docs:** [README.md](../README.md), [README.md](README.md), [OAUTH.md](OAUTH.md), [mail_mode.md](mail_mode.md)

## Getting Started

`std::slop` needs at least one authentication method before it can talk to an LLM. Choose one of these setup paths:

1. **Gemini with a Google API key**
   - Best if you already have a Google AI Studio key.
   - Start with an environment variable or put the key in `config.ini`.
2. **OpenAI-compatible API key**
   - Best if you want to use OpenAI or an OpenAI-compatible provider such as OpenRouter.
   - Start with an environment variable, CLI flag, or `config.ini`.
3. **OpenAI OAuth**
   - Best if you want to use ChatGPT Plus/Pro OAuth with the built-in bootstrap flow.
   - First fetch a token with one of:
     - `std_slop --fetch_openai_oauth_token`
     - `std_slop --fetch_openai_oauth_device_token`
   - Then run `std_slop --openai_oauth`.

After choosing a path, launch `std_slop` with one of the examples below.

## Install and Build

If you do not want to build from source, you can also install pre-built macOS and Linux x86-64 binaries from the GitHub releases page: https://github.com/hsaliak/std_slop/releases

1. **Clone the project**
   ```bash
   git clone https://github.com/hsaliak/std_slop.git
   cd std_slop
   ```
2. **Ensure [Bazel](https://bazel.build/) is installed**
   ```bash
   bazel test //...
   bazel build //...
   ```
3. **Copy the binary somewhere on your `PATH`**
   ```bash
   cp bazel-bin/app/std_slop "$HOME/bin/std_slop"
   ```

## Authentication and Configuration Paths

### Option 1: Gemini via environment variable

```bash
export GOOGLE_API_KEY="your-google-api-key"
std_slop
```

You can also choose a model explicitly:

```bash
std_slop --google_api_key "$GOOGLE_API_KEY" --model gemini-3-flash-preview
```

### Option 2: OpenAI-compatible API key

Use the OpenAI API directly:

```bash
export OPENAI_API_KEY="your-openai-api-key"
std_slop --model gpt-5.4-mini:high
```

Or point `std::slop` at an OpenAI-compatible provider such as OpenRouter:

```bash
export OPENAI_API_KEY="your-openrouter-key"
std_slop --openai_base_url https://openrouter.ai/api/v1 --model openai/gpt-4o-mini
```

### Option 3: OpenAI OAuth

Use one of the built-in bootstrap flows to save an OAuth token locally.

#### Browser + paste flow

```bash
std_slop --fetch_openai_oauth_token
std_slop --openai_oauth
```

#### Headless / device flow

```bash
std_slop --fetch_openai_oauth_device_token
std_slop --openai_oauth
```

By default, the token is stored under `~/.config/slop/`.

## Setting Up `config.ini`

If you do not want to pass keys on every command, create a config file at:

```text
~/.config/slop/config.ini
```

Create the directory and copy the example config:

```bash
mkdir -p ~/.config/slop
cp docs/example_config.ini ~/.config/slop/config.ini
```

A minimal Gemini config looks like this:

```ini
[slop]
google_api_key = ${GOOGLE_API_KEY}
model = gemini-3-flash-preview
```

A minimal OpenAI-compatible config looks like this:

```ini
[slop]
openai_api_key = ${OPENAI_API_KEY}
model = gpt-5.4-mini:high
```

An OpenRouter-style config looks like this:

```ini
[slop]
openai_api_key = ${OPENAI_API_KEY}
openai_base_url = https://openrouter.ai/api/v1
model = openai/gpt-4o-mini
```

If you already fetched an OpenAI OAuth token, you can use:

```ini
[slop]
openai_oauth = true
model = gpt-5.4-mini:high
```

Run with the default config path:

```bash
std_slop
```

Or point to a specific file:

```bash
std_slop --config ~/.config/slop/config.ini
```

## First Session

Once authentication is set up, start a session and ask for something simple:

```bash
std_slop --prompt "Summarize the repository structure"
```

Or launch the interactive UI:

```bash
std_slop
```

Helpful first commands inside the UI:

```text
/models
/help
/session switch scratch
```

If you want to explore `std::slop`-specific workflows and built-in personas, browse the `docs/` folder. Good next reads include [mail_mode.md](mail_mode.md) for patch-based development, [mail-loop/README.md](mail-loop/README.md) for the automated mail-loop workflow, and [README.md](README.md) for the full docs map.

## Configuring `llm_query` Subqueries and Personas

`std::slop` can define focused subquery tools from INI config. These are useful when you want a reusable delegated persona for tasks like code review, code exploration, or summarization.

Add `[llm_tool_*]` sections to a config file to register specialized `llm_query` tools. Each section gives the subquery a tool name, a prompt patch, a session ID, and a skill/persona name to activate during delegation.

Minimal example:

```ini
[llm_tool_code_reviewer]
system_prompt_patch = You are a strict code reviewer. Focus on required changes only.
session_id = code_review
skill = code_reviewer
context_window = 8
```

Run with a config that includes these sections:

```bash
std_slop --config ~/.config/slop/config.ini
```

For a complete example, see [example_subqueries.ini](example_subqueries.ini). For more detail on the config shape and policy boundaries, see [impl/subqueries.md](impl/subqueries.md).

## Troubleshooting

If `std::slop` says no authentication method is configured, go back to **Getting Started** and choose one of the setup paths above.

For verbose logs:

```bash
bazel run //:std_slop -- --log=debug.log
bazel run //:std_slop -- --stderrthreshold=0
bazel run //:std_slop -- --v=2 --stderrthreshold=0
```

## Next Steps

- Read [README.md](README.md) for the full docs map.
- Read [OAUTH.md](OAUTH.md) for OpenAI OAuth details.
- Read [mail_mode.md](mail_mode.md) for the patch-based development workflow.
