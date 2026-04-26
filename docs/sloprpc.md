
# slop RPC Service (`slop_rpc_service`) — Detailed Implementation Plan

This document defines a minimal, forward-only plan to add a gRPC server entrypoint
that reuses `std_slop` runtime fundamentals while remaining strictly non-interactive.

## 1) Scope and Goals

### Goals
- Add a new binary: `//rpc:slop_rpc_service`.
- Reuse the same execution core as `std_slop --prompt`.
- Expose a structured unary RPC API for higher-level orchestrators.
- Enforce non-interactive behavior (no terminal input, no `ask_user`).
- Use proto-based per-server configuration for RPC via `server.cfg`.

### Non-goals (MVP)
- Streaming RPCs.
- Bidirectional orchestration protocol.
- Backward compatibility shims for old RPC contracts.

## 2) Two-Program Configuration Model

We intentionally maintain separate config surfaces:

- `//app:std_slop`
  - Continues using `config.ini` + existing CLI flags.

- `//rpc:slop_rpc_service`
  - Uses `--server_config=/path/server.cfg`.
  - `server.cfg` is textproto encoded against `rpc/server_config.proto`.

This keeps CLI behavior stable while giving the RPC server a dedicated, explicit,
schema-validated configuration model.

## 3) RPC API Contract (MVP)

Create `rpc/slop_rpc.proto` with one unary method.

```proto
syntax = "proto3";

package slop.rpc.v1;

service SlopService {
  rpc RunPrompt(RunPromptRequest) returns (RunPromptResponse);
}

message RunPromptRequest {
  string prompt = 1;                    // required
  string session_id = 2;                // optional
  repeated string active_skills = 3;    // optional
  string model_override = 4;            // optional allowlisted override
  optional int32 context_window = 5;    // optional allowlisted override
}

message RunPromptResponse {
  bool success = 1;
  string content = 2;
  string session_id = 3;
  string error_code = 4;
  string error_message = 5;
}
```

`ExecutionScope` is intentionally **not** part of the external RPC request for MVP.
Subquery behavior is treated as an internal agent concern controlled by server policy.

## 4) Server Config Schema (RPC-only)

Create `rpc/server_config.proto` for process-level server configuration.

```proto
syntax = "proto3";

package slop.rpc.v1;

message ProviderConfig {
  string provider = 1;                  // e.g. "gemini", "openai"
  string model = 2;
  string openai_api_key_env = 3;        // env var names (not key literals)
  string gemini_api_key_env = 4;
  bool openai_oauth = 5;
}

message RequestPolicy {
  bool disable_ask_user = 1;            // default true
  bool allow_request_model_override = 2;
  bool allow_request_skill_override = 3;
  bool allow_request_context_window_override = 4;
  int32 max_context_window = 5;         // 0 = unlimited
  // max_execution_depth removed; RPC currently supports one internal subquery hop.
}

message RequestDefaults {
  repeated string active_skills = 1;
  optional int32 context_window = 2;
}

// Mirrors INI sections like [llm_tool_code_review_llm] in docs/example_subqueries.ini.
message LlmToolSpecialization {
  string name = 1;                 // e.g. "code_review_llm"
  string system_prompt_patch = 2;
  string session_id = 3;
  string skill = 4;
  optional int32 context_window = 5;
}

message ServerConfig {
  string listen_addr = 1;               // default "0.0.0.0:50051"
  string db_path = 2;                   // supports ":memory:" for in-memory DB
  ProviderConfig provider = 3;
  RequestPolicy policy = 4;
  RequestDefaults defaults = 5;
  repeated LlmToolSpecialization llm_tool_specializations = 6;
}
```

### Specialization parity target (INI -> proto)

The RPC server config must express the same specialization intent currently supported by
`docs/example_subqueries.ini`:

- INI section `[llm_tool_<name>]` -> `llm_tool_specializations { name: "<name>" ... }`
- `system_prompt_patch` -> `system_prompt_patch`
- `session_id` -> `session_id`
- `skill` -> `skill`
- `context_window` -> `context_window`

At runtime, both sources should map to one shared internal registration path.

### In-memory DB support

`db_path` must accept:
- `:memory:` for ephemeral in-memory operation.
- regular file paths for persistent state.

For agent chaining persistence across requests, default should be a persistent file path;
`:memory:` is intended for isolated test/dev deployments.

## 5) `server.cfg` Loading and Precedence

`server.cfg` is textproto parsed as `ServerConfig`.

Precedence for `slop_rpc_service`:
1. CLI flags
2. `server.cfg`
3. environment
4. hardcoded defaults

Per-request overrides are allowlisted and policy-gated.

Example: if `allow_request_model_override=false`, `model_override` in
`RunPromptRequest` is rejected with `INVALID_ARGUMENT`.

Server config (`server.cfg`) controls internal request policy. Callers do not set execution scope/depth directly in RPC.
RPC currently supports one internal subquery hop; this is enforced server-side
and is not configurable in `server.cfg`.

## 6) Shared Runtime Bootstrap (Required Refactor)

Extract runtime assembly currently in `app/main.cpp` into shared helpers,
for example under `app/runtime_bootstrap.{h,cpp}`.

### New reusable types
- `RuntimeOptions` (db path, provider/model defaults, oauth/key behavior, etc.)
- `ExecutionDefaults` (skills/context window defaults)
- `RuntimeBundle` (db/http/orchestrator/tool executor/interaction engine)

### Adapters
- CLI adapter (`config.ini` + flags) -> `RuntimeOptions`.
- RPC adapter (`ServerConfig` + flags) -> `RuntimeOptions`.

## 7) Non-interactive Policy Enforcement

For all RPC requests:
- Always run silent/non-interactive.
- Disable `ask_user` tool in server execution context.
- Reject attempts to execute interactive-only command/tool paths.

This mirrors sub-agent safety and keeps RPC deterministic.

## 8) Bazel / Dependency Plan

### MODULE.bazel
Add protobuf/gRPC dependencies needed for C++ service and generated code.

### New package: `rpc/`
- `slop_rpc.proto`
- `server_config.proto`
- server main/service implementation files
- `BUILD.bazel` with:
  - proto/grpc codegen targets
  - `cc_binary(name = "slop_rpc_service", ... )`
  - unit/integration tests

## 9) Implementation Plan (Bundled Order)

### Bundle 1: Shared Runtime Bootstrap Extraction
- Implement
  - Extract runtime assembly from `app/main.cpp` into reusable
    `app/runtime_bootstrap.{h,cpp}`.
  - Keep behavior unchanged for `//app:std_slop`.
- Validate
  - `bazel build //app:std_slop`
  - existing prompt-mode smoke check still passes.

### Bundle 2: Proto Config Plumbing (`server.cfg`)
- Implement
  - Add `rpc/server_config.proto` with provider/policy/defaults and
    `llm_tool_specializations`.
  - Add parser/validator for `--server_config=/path/server.cfg`.
  - Add adapter: `ServerConfig` -> runtime options.
  - Register `llm_tool_specializations` via the same runtime registration path
    used by INI specializations.
- Validate
  - unit tests for config parsing + precedence.
  - unit tests for specialization mapping parity with `docs/example_subqueries.ini`.

### Bundle 3: Minimal RPC Surface
- Implement
  - Add `rpc/slop_rpc.proto` with unary `RunPrompt`.
  - Keep external request minimal (`prompt`, optional `session_id`, optional
    `active_skills`, policy-gated overrides).
  - Do not expose `ExecutionScope` in request.
- Validate
  - `bazel build //rpc:slop_rpc_service`
  - request validation tests (`prompt` required, disallowed overrides rejected).

### Bundle 4: Non-interactive Enforcement + Chaining Readiness
- Implement
  - Disable `ask_user` for RPC execution path.
  - Enforce internal subquery depth/safety via server policy (single subquery hop).
  - Preserve stateful chaining via persistent DB mode; support `:memory:` for
    isolated deployments/tests.
- Validate
  - unit tests ensuring no interactive path is reachable from RPC.
  - smoke test with `docs/impl/rpc/server.cfg` and
    `docs/impl/rpc/server_in_memory.cfg`.

### Bundle 5: Final Validation + Docs Lock
- Implement
  - finalize docs/examples for operator usage and orchestration callers.
- Validate
  - `bazel build //app:std_slop //rpc:slop_rpc_service`
  - targeted tests in `//rpc` + bootstrap-related tests.

## 10) Test Plan (minimum required)

### Unit tests
- `server_config` parsing and precedence.
- invalid request rejection (`prompt` missing, disallowed overrides).
- non-interactive enforcement (`ask_user` disabled).

### Integration/smoke
- start `slop_rpc_service` with `docs/impl/rpc/server.cfg`.
- call `RunPrompt` and assert structured success response.
- run with `docs/impl/rpc/server_in_memory.cfg` and verify in-memory mode boots.

### Build checks
- `bazel build //app:std_slop //rpc:slop_rpc_service`
- relevant `bazel test` targets for rpc + runtime bootstrap.

## 11) Operator Quickstart (Bundle 5 docs lock)

### Build + start server

```bash
bazel build //rpc:slop_rpc_service
./bazel-bin/rpc/slop_rpc_service --server_config=docs/impl/rpc/server.cfg
```

Use the in-memory profile for isolated testing:

```bash
./bazel-bin/rpc/slop_rpc_service --server_config=docs/impl/rpc/server_in_memory.cfg
```

### Call `RunPrompt` with grpcurl

```bash
grpcurl -plaintext \
  -proto rpc/slop_rpc.proto \
  -d '{"prompt":"Say hello from rpc","session_id":"rpc-demo"}' \
  127.0.0.1:50051 slop.rpc.v1.SlopService/RunPrompt
```

Expected response shape:

```json
{
  "success": true,
  "content": "...assistant text...",
  "sessionId": "rpc-demo",
  "errorCode": "",
  "errorMessage": ""
}
```

For failures (`success=false`), callers must treat `errorCode` + `errorMessage` as the
authoritative machine/human error pair.

### Caller contract notes

- `prompt` is required.
- `model_override`, `active_skills`, and `context_window` are policy-gated per `server.cfg`.
- RPC is strictly non-interactive (`ask_user` is disabled by server policy).

## 12) Rollout Sequence

1. Land shared runtime bootstrap extraction.
2. Land proto config model (`server_config.proto`) + `server.cfg` loader,
   including `llm_tool_specializations` parity with INI.
3. Land RPC service proto + unary server (without external `ExecutionScope`).
4. Land non-interactive policy + tests.
5. Land docs/examples.
