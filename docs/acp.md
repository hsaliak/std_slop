
# ACP Integration Plan (Server Mode)

## Goal

Add **basic ACP support** to this codebase with a simple, maintainable implementation that reuses existing std::slop execution paths.

This plan reflects the following product constraints:

- Keep ACP implementation simple; no advanced features.
- Keep ACP code isolated in `acp/` as much as possible.
- Add a dedicated CLI mode via `--acp`.
- `--acp` must be mutually exclusive with `--prompt` and default interactive mode.
- Build ACP support directly in C++ (no schema codegen, no Rust sidecar/FFI).
- Deliver work as **feature bundles**, where implementation + unit tests + fuzz tests are developed together.

---

## Design Invariants

1. **ACP surface isolation**
   - New ACP runtime, transport, routing, method handlers, and protocol types live under `acp/`.
   - Existing modules (`interface/`, `core/`, `tools/`) are reused as dependencies, not restructured for ACP.

2. **Mode gating**
   - Add `--acp` boolean flag in `app/main.cpp`.
   - Validation rule:
     - `--acp` + `--prompt` => invalid usage (fail fast with clear error)
     - `--acp` set => run ACP server loop and do not enter interactive loop.

3. **Stable protocol only**
   - Implement ACP stable v1 methods from `schema/meta.json` only.
   - Ignore/return unsupported for unstable methods (`schema/meta.unstable.json`).

4. **Session identity mapping**
   - ACP session lifecycle maps to std::slop DB-backed sessions.
   - For MVP, prefer ACP session id == internal session id when possible.

5. **No true token streaming requirement**
   - `session/update` support is interpreted as progress notifications.
   - Final result still returned as the normal request response.

6. **Cancellation must reuse existing flow**
   - ACP `session/cancel` hooks into existing `CancellationRequest` path used by dispatcher/executor.

---

## Scope (MVP)

### Methods in scope

- `initialize`
- `session/new`
- `session/prompt`
- `session/cancel`
- `session/update` (notifications, non-token-streaming)

### Out of scope (explicitly)

- Unstable ACP methods
- Document/NES advanced surfaces
- HTTP transport
- Code generation from ACP schema
- Rust ACP sidecar/FFI

---

## Proposed Module Layout

All new code under `acp/` unless noted.

- `acp/server.h|cpp`
  - ACP runtime loop, request dispatch orchestration, lifecycle.

- `acp/transport_stdio.h|cpp`
  - Stdio JSON-RPC framing (newline-delimited JSON); optional `poll()`-based loop if needed.

- `acp/rpc_envelope.h|cpp`
  - Parse/validate JSON-RPC envelopes and build responses/notifications.

- `acp/method_router.h|cpp`
  - Method-to-handler dispatch (`initialize`, `session/*`).

- `acp/capabilities.h|cpp`
  - Protocol version and server capability negotiation.

- `acp/session_service.h|cpp`
  - ACP-to-DB session lifecycle handling.

- `acp/engine_adapter.h|cpp`
  - Bridge ACP prompt call to existing `InteractionEngine`/orchestrator flow.

- `acp/request_registry.h|cpp`
  - Track in-flight requests and cancellation handles.

- `acp/update_publisher.h|cpp`
  - Emit `session/update` progress notifications.

- `acp/error_mapping.h|cpp`
  - Central mapping of internal statuses to ACP/JSON-RPC error payloads.

- `acp/BUILD.bazel`
  - ACP library, unit test, and fuzz targets.

Small integration touchpoints outside `acp/`:

- `app/main.cpp`:
  - add `--acp` flag
  - enforce mode exclusivity
  - branch to ACP server mode

---

## Feature Bundles (implementation + unit tests + fuzzing together)

## Bundle 1: ACP runtime shell + mode wiring

### Implementation

- Add `--acp` flag and mutual exclusivity checks in `app/main.cpp`.
- Introduce ACP server entrypoint and stdio loop skeleton.
- Add envelope parser/writer with strict request-shape validation.

### Unit tests

- CLI mode validation tests (`--acp` conflicts with `--prompt`).
- JSON-RPC envelope parsing/serialization success and error cases.
- Unknown method returns method-not-found.

### Fuzz tests

- Fuzz JSON-RPC envelope parser:
  - no crash
  - malformed input rejected cleanly
  - malformed shape does not reach handlers

### Exit criteria

- ACP server loop can start/stop cleanly.
- Invalid envelopes produce deterministic error responses.

---

## Bundle 2: Handshake and capabilities (`initialize`)

### Implementation

- Implement `initialize` handler with stable v1 checks.
- Return minimal declared capabilities for MVP.
- Persist negotiated runtime options in ACP server state.

### Unit tests

- valid `initialize` request succeeds.
- unsupported version rejected with stable error shape.
- missing/invalid params rejected.

### Fuzz tests

- Fuzz `initialize` params object and capability fields.

### Exit criteria

- Negotiation is deterministic and version-gated.

---

## Bundle 3: Session lifecycle (`session/new`) mapped to std::slop sessions

### Implementation

- Implement `session/new` using existing DB session model.
- Ensure returned session IDs are valid for prompt/cancel paths.

### Unit tests

- session creation success path.
- invalid args and malformed IDs rejected.
- session identity usable across method calls.

### Fuzz tests

- Fuzz `session/new` payload shapes and edge-case IDs.

### Exit criteria

- ACP session creation is backed by real std::slop session state.

---

## Bundle 4: Prompt execution (`session/prompt`) via existing engine path

### Implementation

- Add `engine_adapter` that invokes existing `InteractionEngine` behavior.
- Map ACP prompt input to internal query/process call.
- Map final model output to ACP response payload.

### Unit tests

- prompt success path for valid session.
- invalid/missing session behavior.
- status/error mapping for engine failures.

### Fuzz tests

- Fuzz prompt payload structure and large input strings.

### Exit criteria

- ACP prompt request executes through existing orchestrator flow without rewrite.

---

## Bundle 5: Cancellation (`session/cancel`) integrated with existing cancellation flow

### Implementation

- Track in-flight prompt requests in request registry.
- Wire `session/cancel` to existing `CancellationRequest` path.
- Return cancelled terminal status (not generic internal failure).

### Unit tests

- cancel active request -> cancelled result.
- cancel unknown request -> deterministic error.
- cleanup/registry removal on completion and cancellation.

### Fuzz tests

- Fuzz cancel payloads.
- Fuzz interleaved prompt/cancel message orderings.

### Exit criteria

- Cancellation is reliable and reuses dispatcher/executor cancellation mechanisms.

---

## Bundle 6: `session/update` progress notifications (non-streaming)

### Implementation

- Emit coarse updates at key checkpoints, for example:
  - accepted
  - started
  - executing tools (if applicable)
  - completed/cancelled
- Keep final output in normal response.

### Unit tests

- expected update ordering and shape.
- completion/cancellation terminal updates.

### Fuzz tests

- Fuzz notification payload shaping from internal state transitions.

### Exit criteria

- ACP clients receive useful progress events without token streaming support.

---

## Error Handling and Validation Rules

- Treat ACP request payloads as untrusted input.
- Validate method + params shape at ACP router boundary before side effects.
- Centralize error translation in `acp/error_mapping.*`.
- Use `core/json_utils.h` helpers (`json_parse`, `json_get`, `json_get_or`, `json_dump`) in production ACP parsing/serialization paths.

---

## Dependency Strategy

### Chosen approach

- Implement ACP directly in this C++ codebase.
- Reuse existing dependencies and threading/cancellation primitives.
- No new major dependency required.

### Explicit rejections

- No ACP schema code generation pipeline.
- No Rust crate embedding/sidecar/FFI.

---

## Rollout and Merge Strategy

- Land each bundle independently with:
  - implementation
  - unit tests
  - fuzz tests
  - BUILD updates
- Do not proceed to next bundle unless current bundle is green in CI.
- Keep ACP patches tightly scoped to reduce integration risk.

---

## Technical Anchors

- Mode control entrypoint: `app/main.cpp`
- Session/query execution path: `interface/interaction_engine.h`
- Cancellation-aware dispatch path: `tools/tool_dispatcher.cpp`
- Tool execution boundary: `tools/tool_executor.h`
- Session persistence: `core/database.h`
- ACP stable schema: `../agent-client-protocol/schema/meta.json`
- ACP unstable schema (excluded): `../agent-client-protocol/schema/meta.unstable.json`