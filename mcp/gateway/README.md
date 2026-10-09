# `run_js_server` example gateway

`run_js_server` is a stdio MCP server that runs bounded JavaScript and calls tools from configured downstream stdio MCP servers. It exposes two tools:

- `run_js`: run JavaScript with optional JSON input.
- `run_js_help`: list authorized tools or inspect one tool's schema.

This file is the user setup guide. Current implementation status and follow-up work are in [STATUS.md](STATUS.md).

Inside JavaScript, call an authorized tool with `await alias.tool(args)` or `await mcp.call("alias", "tool", args)`. Use `help()` or `mcp.help()` to inspect the authorized catalog.

## Example: use the gateway from `std_slop`

This example uses the echo MCP server. Build the gateway, echo server, and `std_slop`; use absolute executable paths in the files below.

Create `gateway.json`:

```json
{
  "runTimeoutMs": 30000,
  "traceLogPath": "/tmp/run_js_trace.log",
  "servers": [
    {
      "alias": "echo",
      "transport": "stdio",
      "command": "/absolute/path/to/bazel-bin/mcp/server/echo_server",
      "args": [],
      "allowTools": ["echo"]
    }
  ]
}
```

Register the gateway in the std_slop MCP registry, normally `~/.config/slop/mcp.ini`:

```ini
[server.js]
transport=stdio
command=/absolute/path/to/bazel-bin/mcp/gateway/run_js_server
args_json=["--config","/absolute/path/to/gateway.json"]
enabled=true
```

Start `std_slop` with this registry and your usual model configuration. The registry alias `js` exposes the tools `mcp_js_run_js` and `mcp_js_run_js_help`. Ask the model to call `mcp_js_run_js` with this code:

```javascript
const result = await echo.echo({text: "hello from std_slop"});
return result.structuredContent;
```

The returned `structuredContent.result` should be `{"text":"hello from std_slop"}`. A non-empty `allowTools` list restricts JavaScript to the listed tools. If `allowTools` is omitted or empty, all tools discovered from that downstream server are available.

This example enables a full trace log. In another tmux pane, follow it with:

```bash
tail -F /tmp/run_js_trace.log
```

The log contains the JavaScript source, run input, downstream tool calls and arguments, raw and JavaScript-visible results, and the final run result or error. Each record includes a timestamp and trace identifier.

To check the tools from JavaScript, call:

```javascript
const schema = help("echo", "echo");
const echoed = await echo.echo({text: "hello"});
return {schema, result: echoed.structuredContent};
```

To check repeated calls and sequential awaits:

```javascript
const messages = ["loop-1", "loop-2", "loop-3"];
const replies = [];
for (const text of messages) {
  const response = await echo.echo({text});
  replies.push(response.structuredContent);
}
return {
  count: replies.length,
  replies,
  allMatched: replies.every((reply, index) => reply.text === messages[index])
};
```

The result should report `count: 3` and `allMatched: true`.

## Configuration and limits

Optional gateway configuration options:

- `runTimeoutMs`: Run deadline in milliseconds. The default is 30000 ms; the allowed range is 1–60000 ms.
- `traceLogPath`: Absolute path for the trace log file. Omit this option to disable trace logging.

The gateway also enforces limits on code, QuickJS heap and stack, result size, tool-call count, catalog size, and queued calls. The configuration accepts stdio servers with absolute command paths and literal argument arrays. Set a non-empty `allowTools` list to restrict the exposed tools; omitting it or setting it to `[]` exposes every discovered tool from that server. It does not accept HTTP servers or secrets.

## Example: connect to the Decision API server

Add the Decision API server as a downstream stdio server in `gateway.json` (keep other server entries you need):

```json
{
  "runTimeoutMs": 30000,
  "servers": [
    {
      "alias": "decisions",
      "transport": "stdio",
      "command": "/absolute/path/to/bazel-bin/mcp/decision_api/decision_api_server",
      "args": ["--config", "/absolute/path/to/.config/slop/decision-api.json"],
      "allowTools": ["decision_help", "decision_models", "decide"]
    }
  ]
}
```

The `decision-api.json` file holds the API key and model for the configured Decision API service (OpenRouter is the default); do not put credentials in `gateway.json` or JavaScript. See the [Decision API setup guide](../decision_api/README.md) for its format and permission requirements. In `~/.config/slop/mcp.ini`, register the gateway using the registry alias `run_js`:

```ini
[server.run_js]
transport=stdio
command=/absolute/path/to/bazel-bin/mcp/gateway/run_js_server
args_json=["--config","/absolute/path/to/gateway.json"]
enabled=true
```

The agent calls `mcp_run_js_run_js` with JavaScript in its `code` argument. The JavaScript alias is `decisions`:

```javascript
const guide = await decisions.decision_help({topic: "overview"});
const models = await decisions.decision_models({});
const result = await decisions.decide({
  state: {message: "The invoice was paid yesterday."},
  questions: {
    contains_keyword: {
      type: "noul",
      instructions: "Does the message contain the exact word invoice?",
      criteria: {
        true: "The message contains the word invoice.",
        false: "The message does not contain the word invoice."
      }
    }
  }
});
return {
  help: guide.structuredContent,
  modelCount: models.structuredContent.total_count,
  decision: result.structuredContent
};
```

Use `allowTools` to expose only the tools required by the script. `traceLogPath` is optional. When enabled, the gateway logs code, arguments, and results, including decision state; disable tracing or use non-sensitive data for tests. To also call the server directly, register it separately under `[server.decisions]`; the direct tool names then start with `mcp_decisions_`. See the [Decision API setup guide](../decision_api/README.md).

Every `run_js` call runs in a fresh `run_js_server --worker-fd` child. The parent and worker exchange length-prefixed JSON frames over a private Unix socketpair, with a 4 MiB frame limit. The child receives code, input, the authorized public catalog, limits, and tool results. The parent validates worker requests and performs all downstream MCP calls. Calls to one downstream client run serially; calls to different clients may run concurrently. The remaining run deadline is passed to downstream calls.

## Testing without an LLM

Build the example servers and run the subprocess integration test:

```bash
bazel build //mcp/gateway:run_js_server //mcp/server:echo_server
bazel test //mcp/gateway:gateway_subprocess_test --test_output=all
```

The test composes concurrent echo calls through the worker and verifies that a 100 ms run timeout stops an infinite loop. Run `bazel test //...` for the full repository suite.

## Security considerations

An empty or omitted `allowTools` list grants JavaScript access to every tool advertised by that downstream server. Use a non-empty list when you need least-privilege access. Configured downstream commands run with the gateway account's operating-system permissions.

Trace logging is opt-in. When `traceLogPath` is set, the gateway logs code, run input, tool arguments, and raw and normalized tool results. Source code is preserved except that terminal control bytes are escaped for safe display. These values may contain secrets. The log file is created with mode `0600`, but it grows as runs are recorded; protect, rotate, and delete it as needed. Logging is for debugging, not an audit trail.

Each run uses a worker process, but this is not an OS sandbox. A QuickJS or native-library exploit could compromise the worker and access resources available to the gateway account. Do not pass secrets to the gateway unless you accept that they may appear in configured trace logs. The gateway does not execute application handlers or provide reverse calls into the host agent. To expose application capabilities, use a separate MCP server that preserves their existing authorization checks and configure it as a downstream server.
