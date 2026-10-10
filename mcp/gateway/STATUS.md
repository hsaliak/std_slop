# Gateway implementation status

For the user setup example, see the [run_js gateway guide](README.md).

## Current state

- Bundle 1: bounded QuickJS runtime, safe JSON conversion, async MCP calls, unit tests and fuzz targets are implemented.
- Bundle 2: stdio and Streamable HTTP MCP gateway, strict transport-specific config parsing, catalog/help, normalized results, subprocess tests, and config/result fuzz coverage are implemented. HTTP clients use automatic protocol selection. An omitted or empty `allowTools` list exposes all discovered tools; a non-empty list restricts access.
- Bundle 3: one FIFO worker lane per downstream Client, up to four active calls and 64 outstanding calls, per-lane Client ownership, and run/call correlation are implemented.
- Bundle 4: each `run_js` request starts a fresh `run_js_server --worker-fd` process. Parent and worker use a bounded, length-prefixed JSON protocol over a private Unix socketpair. The worker receives code, input, the authorized public catalog, limits, and call results. Only the parent performs downstream MCP calls. The parent validates worker requests and matches run/call IDs. Runs have one monotonic deadline; remaining time reaches argument validation, MCP Client, and transport send/receive. Worker shutdown terminates and reaps the child.
- Worker and parent do not share the outer MCP standard streams or the parent environment. Linux uses `posix_spawn` descriptor close-from support; other POSIX builds use a bounded descriptor scan. The worker process boundary is not an OS sandbox. The operator confirmed no Linux/macOS confinement policy is required for this bundle.

## Verification

The real subprocess test composes stdio and local classic Streamable HTTP downstream calls through worker IPC, and checks an infinite-loop script returns a budget error under a 100 ms configured run deadline. Worker IPC parsing has unit and fuzz tests. JSON Schema validation covers the supported Draft 7 and 2020-12 subsets. `bazel test //...` passed all 110 test targets. Targeted TSAN passed worker subprocess, worker unit, scheduler, gateway, and MCP client/session tests. `git diff --check` passed.

## Follow-up work

1. Verify worker spawn/descriptor behavior on macOS CI; Linux is the current execution platform.
2. Add an approved application-tools MCP adapter only after its existing authorization boundary is identified. Do not auto-export application handlers or credentials.
3. OS-level confinement is not a Bundle 4 requirement per operator guidance. Do not claim the process worker is a sandbox.
