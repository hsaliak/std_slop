# Local workspace MCP smoke test

This package provides the stdio MCP server used by the smoke test. For the full tool reference and security behavior, see the [agent MCP guide](../../docs/mcp-slop-userguide.md#bundled-local-workspace-tools).

## Smoke test

This test checks the actual `std_slop` → local-tools MCP stdio path in one Git directory and one non-Git directory. It uses temporary marker files.

For a model-driven smoke call, configure an OpenRouter API key in `~/.config/slop/config.ini`, set `openai_base_url = https://openrouter.ai/api/v1`, and set `openai_oauth = false`. Do not put the key on the command line. `openrouter/free:medium` is a convenient free-route model for basic tool-call tests when OpenRouter makes it available. You can use another configured model that supports tools.

From the repository root, build the server and prepare the test folders. Pick an unused MCP registry name; the example uses `local_smoke`:

```sh
bazel build //mcp/local_tools:local_tools_server
SERVER="$(pwd)/bazel-bin/mcp/local_tools/local_tools_server"
SMOKE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/slop-mcp-smoke.XXXXXX")"
GIT_ROOT="$SMOKE_ROOT/git-root"
PLAIN_ROOT="$SMOKE_ROOT/plain-root"
mkdir -p "$GIT_ROOT" "$PLAIN_ROOT"
git -C "$GIT_ROOT" init --quiet -b main
git -C "$GIT_ROOT" config user.name "MCP smoke test"
git -C "$GIT_ROOT" config user.email "mcp-smoke@example.invalid"
printf 'GIT_ROOT_SMOKE\n' > "$GIT_ROOT/marker.txt"
printf 'PLAIN_ROOT_SMOKE\n' > "$PLAIN_ROOT/marker.txt"
git -C "$GIT_ROOT" add marker.txt
git -C "$GIT_ROOT" commit --quiet -m "seed MCP smoke root"
std_slop mcp add local_smoke --transport stdio --command "$SERVER"
std_slop mcp list
```

Run the following command once from each root. Starting `std_slop` from a root also makes that directory the MCP server's launch workspace. To run both from one shell, `cd "$GIT_ROOT"`, run it, then `cd "$PLAIN_ROOT"` and run it again. If you are inside tmux, you can first open detached windows with `tmux new-window -d -n mcp-smoke-git -c "$GIT_ROOT"` and `tmux new-window -d -n mcp-smoke-plain -c "$PLAIN_ROOT"`.

```sh
std_slop --model openrouter/free:medium \
  --prompt 'Call MCP tool mcp_local_smoke_workspace_info and mcp_local_smoke_read_file with path marker.txt. Return the workspace root and exact file contents.' \
  --output json
```

The Git run should report `$GIT_ROOT` and `GIT_ROOT_SMOKE`; the non-Git run should report `$PLAIN_ROOT` and `PLAIN_ROOT_SMOKE`. A non-Git directory is supported; its workspace list contains only `launch`. This command does not pass `--no_local_tools`, so it tests the normal catalog with native and MCP tools enabled. Add `--no_local_tools` only when you specifically want to test MCP-only mode.

### Mail Mode

Keep the Git root clean and keep the database outside it. From `$GIT_ROOT`, start `std_slop --db "$SMOKE_ROOT/mail.db"`, enter `/mode mail mcp-smoke`, and let it create a `slop/staging/*` branch. Then exit the interactive app and run this prompt from the same root:

```sh
std_slop --prompt_db "$SMOKE_ROOT/mail.db" --model openrouter/free:medium \
  --prompt 'Call MCP tool mcp_local_smoke_workspace_info and mcp_local_smoke_read_file with path marker.txt. Return the workspace root and exact file contents.' \
  --output json
```

Do not pass `--no_local_tools` in this run; it checks that MCP tools work in Mail Mode while native tools remain enabled. Dynamically imported MCP calls require the staging branch.

### Cleanup

After checking the output, remove the temporary registration and test folders:

```sh
std_slop mcp remove local_smoke
rm -r -- "$SMOKE_ROOT"
```
