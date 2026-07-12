# Responses-Native UX and Orchestrator Improvements

This document is a forward implementation plan for improvements that became practical after std::slop moved to a Responses API-only architecture. It is intended to be worked down bundle by bundle. Each bundle is scoped to produce reviewable patches with explicit tests.

## Current Architectural Baseline

Recent simplifications established the new baseline:

```text
30d7b05 Remove Chat Completions and Gemini orchestrators; default to Responses API only
a16139c Update docs to reflect Responses API-only architecture
a8f25d0 Stabilize prompt material ordering for cache reuse
9881b1d Show Responses prompt-cache usage in the status line
678e851 Relocate active skill patches to input tail for prompt cache stability
126a03f Add stable prompt_cache_key for server-side cache routing
3ad9277 Show context token status
```

Important current properties:

- The request path targets OpenAI Responses only.
- Prompt material is ordered for cache stability:
  1. stable instructions;
  2. stable tools;
  3. history;
  4. dynamic active-skill item;
  5. current request.
- `prompt_cache_key` is computed from stable prompt material.
- `/context show` exposes the latest exact provider input-token count and a clearly labeled local assembled-context estimate.
- The current transport still buffers complete HTTP responses before the interaction engine sees them.
- The code still contains older provider-neutral abstractions such as `OrchestratorStrategy`, even though there is now only one provider payload shape.

## Goals

1. Make the UI responsive while a model response is still being generated.
2. Make Responses concepts native in the core abstractions instead of translating them through older generic message/provider layers.
3. Preserve provider output fidelity for reasoning models and tool continuation.
4. Make request/cache boundaries explicit and testable.
5. Keep each change small enough to validate with focused unit, integration, and fuzz tests.

## Non-Goals

- Do not reintroduce Chat Completions, Gemini, or compatibility layers for removed providers.
- Do not mix compaction, streaming, and strategy removal in a single large refactor.
- Do not persist partial streamed output as final conversation history.
- Do not call local estimates exact token counts unless a provider-compatible tokenizer is added.

## Improvement Catalog

### 1. Typed Responses Event Decoder

The highest-leverage technical foundation is a typed event boundary for Responses events. Today, the system can normalize Responses payloads, but streaming is hard because the transport returns a complete buffered string.

Introduce a Responses event model that both buffered and streamed inputs can use:

```cpp
enum class ResponsesEventType {
  kTextDelta,
  kOutputItemAdded,
  kOutputItemDone,
  kToolCallDelta,
  kToolCallDone,
  kUsage,
  kCompleted,
  kError,
};

struct ResponsesEvent {
  ResponsesEventType type;
  std::string text_delta;
  nlohmann::json output_item;
  std::optional<ResponseUsage> usage;
  std::string error_message;
};
```

The exact fields should be refined during implementation, but callers should not need to inspect raw SSE strings or provider JSON directly.

### 2. Real Streaming Transport

`HttpClient::Post()` currently returns a complete response body. For model generation, add an incremental POST path that delivers response bytes or decoded SSE frames as they arrive.

Preferred direction:

```text
HTTP transport chunks
  -> SSE decoder
  -> Responses event decoder
  -> InteractionEngine event consumer
  -> renderer/status updates
```

Keep the existing buffered `Post()` for OAuth and simple API calls. Add a separate streaming method for model requests rather than changing all call sites at once.

### 3. Incremental Rendering

Once typed events arrive incrementally, render assistant text deltas immediately. Persist only completed normalized output once the provider response completes.

The UI should distinguish:

- text currently streaming;
- tool call pending/running;
- final assistant message committed to history;
- failure/cancellation before completion.

### 4. Typed Turn Lifecycle Status

Replace ad-hoc spinner/status strings with typed turn state:

```cpp
enum class TurnPhase {
  kPreparing,
  kConnecting,
  kWaiting,
  kReceiving,
  kRunningTools,
  kWaitingForFollowUp,
  kCompleted,
  kFailed,
  kCancelled,
};
```

Example rendered lifecycle:

```text
Preparing context…
Connecting…
Waiting for model… 1.8s
Receiving… 245 tokens
Running query_db…
Waiting for follow-up…
Done · Input 12.5k · Cached 10.2k (82%) · Output 1.1k
```

The renderer should consume structured state, not arbitrary strings from transport, orchestrator, and tool code.

### 5. Preserve Responses Output Items

The current message-centric representation can discard Responses-specific metadata. For reasoning and future item types, preserve normalized output items instead of reconstructing everything as plain assistant/tool messages too early.

Metadata to preserve when available:

- output item id;
- output item type;
- assistant phase/status;
- tool-call linkage;
- reasoning summaries;
- encrypted reasoning content;
- provider usage attached to completed responses;
- future item fields that are required for continuation.

Short-term storage can be in-memory for the active turn. Durable multi-turn fidelity may require a later schema change.

### 6. Responses-Native Request Model and Prompt Sections

Make the cache contract explicit in code:

```cpp
struct PromptSections {
  std::string stable_instructions;
  nlohmann::json stable_tools;
  nlohmann::json history;
  nlohmann::json dynamic_skill_item;
  nlohmann::json current_request;
};

struct ResponsesRequest {
  std::string model;
  PromptSections sections;
  std::string prompt_cache_key;
  nlohmann::json rendered_payload;
};
```

Request rendering should visibly preserve this order:

```text
stable instructions
stable tools
history
dynamic skill item
current request
```

This makes prompt-cache behavior easier to test and prevents active-skill changes from accidentally migrating back into the stable prefix.

### 7. Move Database Resolution Outside Payload Serialization

The provider request builder should receive already-resolved tools, skills, selected history, and current request data. It should not query the database while serializing provider JSON.

Benefits:

- request construction becomes unit-testable without database setup;
- cache-key inputs are explicit;
- fewer cross-layer arguments;
- easier streaming and compaction integration;
- fewer hidden side effects during serialization.

### 8. Context Policy Boundary

Accordion context selection currently lives close to orchestration/request assembly. Introduce an explicit policy boundary before adding compaction:

```cpp
class ContextPolicy {
 public:
  virtual absl::StatusOr<ContextWindow> Select(const ContextRequest& request) = 0;
};
```

Potential policies:

- current accordion group selection;
- fixed-window test policy;
- Responses server-side compaction policy;
- explicit `/responses/compact` policy.

The orchestrator should receive selected context rather than deciding both context retention and provider JSON construction.

### 9. Remove Obsolete Strategy Indirection

With one provider architecture, `OrchestratorStrategy` and runtime strategy replacement buy less than they cost. Eventually collapse toward direct Responses composition:

```text
Orchestrator
  ├── ResponsesRequestBuilder
  ├── ResponsesEventDecoder
  ├── ResponsesClient
  └── ContextPolicy
```

Do this after the event and streaming boundaries are defined. Removing indirection first risks refactoring the current buffered architecture and then refactoring again for streaming.

### 10. Additional `/context show` Enhancements

The current implementation already exposes latest exact provider input tokens and a local estimate. Follow-ups can improve presentation and coverage:

- format large counts with separators or compact display;
- show latest cached input tokens and cache hit percentage when available;
- include selected accordion group ids and epoch reset reason;
- add zero-token and large-count tests;
- avoid claiming the estimate is exact unless a provider-compatible tokenizer is added.

## Recommended Bundle Sequence

### Bundle 1: Typed Responses Event Decoder

Purpose: create the event boundary required for streaming without changing UX yet.

Implementation steps:

1. Add a small Responses event model in `core/`, for example `core/responses_events.{h,cpp}`.
2. Extract SSE frame parsing from `OpenAiResponsesOrchestrator` or add it if the current parsing is embedded in response normalization.
3. Normalize both complete JSON responses and SSE event payloads into `ResponsesEvent` values.
4. Keep provider-specific field handling in the Responses decoder/helper, not in UI or database code.
5. Add a narrow API such as:

   ```cpp
   absl::StatusOr<std::vector<ResponsesEvent>> DecodeResponsesEvents(absl::string_view body_or_sse);
   absl::StatusOr<std::optional<ResponsesEvent>> DecodeResponsesSseFrame(absl::string_view frame);
   ```

6. Keep existing buffered behavior intact by adapting current response processing to use the new decoder internally where practical.

Tests:

- Unit tests for text deltas.
- Unit tests for completed output items.
- Unit tests for function/tool call events.
- Unit tests for usage events, including cached input tokens.
- Error event tests.
- Malformed JSON and malformed SSE tests.
- Fuzz target for event decoding because provider responses are untrusted structured input.

Validation commands:

```text
bazel test //core:orchestrator_openai_responses_test
bazel test //core:orchestrator_normalization_fuzz_test
bazel test //core:responses_events_test     # once added
```

Exit criteria:

- Existing buffered generation still works.
- Event decoding is independently testable.
- No UI behavior change is required in this bundle.

### Bundle 2: Streaming HTTP Transport

Purpose: deliver model response data incrementally instead of buffering the whole response.

Implementation steps:

1. Add a streaming model request method to `HttpClient`, leaving `Post()` unchanged for OAuth/simple calls.
2. Use a callback type that can propagate cancellation/failure:

   ```cpp
   using StreamChunkCallback = std::function<absl::Status(absl::string_view chunk)>;

   virtual absl::Status PostStreaming(
       const std::string& url,
       const std::string& body,
       const std::vector<std::string>& headers,
       StreamChunkCallback on_chunk);
   ```

3. Wire libcurl write callbacks to `on_chunk`.
4. Preserve retry/cancellation behavior carefully. Do not retry after partial response bytes have been delivered unless the response is known to be safely restartable.
5. Add clear behavior for callback failure: abort transfer and return that status.
6. Keep transport ignorant of provider semantics; SSE decoding belongs above it.

Tests:

- Unit tests with a fake transport/callback collecting chunks.
- Cancellation test where callback requests abort.
- Error propagation test from callback failure.
- Retry tests for failures before any streamed bytes are delivered.
- No regression in OAuth tests using buffered `Post()`.

Validation commands:

```text
bazel test //core:http_client_test          # if/when present
bazel test //core:cancellation_test
bazel test //core:oauth_handler_test
```

Exit criteria:

- Model call sites can opt into streaming.
- Existing non-streaming callers are unaffected.
- Cancellation does not leave the UI or transport in a stuck state.

### Bundle 3: Responses Streaming Client Integration

Purpose: connect streaming transport to the typed Responses event decoder.

Implementation steps:

1. Add a Responses client layer or method that performs the HTTP request and feeds chunks into an SSE accumulator.
2. Convert completed SSE frames into `ResponsesEvent` values.
3. Expose a callback interface at the event level, not raw chunks:

   ```cpp
   using ResponsesEventCallback = std::function<absl::Status(const ResponsesEvent&)>;
   ```

4. Preserve the final normalized output needed by existing `ProcessResponse` behavior.
5. Ensure usage events are captured for final telemetry and `Database::RecordUsage()`.
6. Ensure error events become user-visible statuses.

Tests:

- Simulated SSE stream with multiple text deltas.
- Tool-call stream followed by completed output.
- Usage event at completion.
- Error event mid-stream.
- Partial frame split across chunks.
- Multiple frames in one chunk.

Validation commands:

```text
bazel test //core:orchestrator_openai_responses_test
bazel test //core:responses_events_test
```

Exit criteria:

- Buffered and streamed event normalization produce equivalent final output for representative fixtures.
- Partial chunks are handled deterministically.

### Bundle 4: Incremental InteractionEngine Rendering

Purpose: show assistant text while it is being generated.

Implementation steps:

1. Add an event-consumer path in `InteractionEngine` for `ResponsesEvent` callbacks.
2. Render `kTextDelta` immediately to the terminal.
3. Buffer deltas in memory until the completed output item arrives.
4. Persist only the completed normalized assistant/tool output, not every partial delta.
5. Ensure markdown rendering does not repeatedly re-render the entire response on every tiny delta unless performance is acceptable.
6. Keep tool calls visually distinct from text deltas.
7. On cancellation, mark the turn cancelled and do not persist an incomplete assistant message as completed.

Tests:

- Interaction test where text deltas are emitted before completion.
- Cancellation during text streaming.
- Tool call emitted after streamed text.
- Persistence test proving only completed normalized output is stored.
- No duplicate assistant output after finalization.

Validation commands:

```text
bazel test //interface:interaction_engine_test
bazel test //interface:ui_test
```

Exit criteria:

- User sees text before the full response completes.
- Conversation history remains clean and final-output based.
- Tool execution still happens at the correct boundary.

### Bundle 5: Typed Turn Lifecycle Status

Purpose: make progress visible and consistent across preparation, network wait, streaming, tools, and completion.

Implementation steps:

1. Add `TurnPhase` and a `TurnStatus` struct in `interface/` or a shared core/interface header:

   ```cpp
   struct TurnStatus {
     TurnPhase phase;
     std::string detail;
     int received_tokens = 0;
     std::optional<ResponseUsage> usage;
     absl::Duration elapsed;
   };
   ```

2. Make `InteractionEngine` own phase transitions.
3. Make the status renderer consume `TurnStatus`.
4. Map phases to concise messages:
   - preparing context;
   - connecting;
   - waiting for model;
   - receiving;
   - running tool;
   - waiting for follow-up;
   - done;
   - failed/cancelled.
5. Include final telemetry:
   - input tokens;
   - cached input tokens;
   - cache percentage;
   - output tokens;
   - elapsed time.
6. Remove competing direct progress prints from lower layers where possible.

Tests:

- Deterministic renderer tests for each phase.
- Cache telemetry formatting tests.
- Failure and cancellation status tests.
- Tool-running status tests.

Validation commands:

```text
bazel test //interface:interaction_engine_test
bazel test //interface:ui_test
```

Exit criteria:

- Status line is driven by typed state.
- Final prompt-cache telemetry remains visible.
- No duplicate/conflicting progress messages appear during a normal turn.

### Bundle 6: Preserve Responses Output Items In Memory

Purpose: avoid losing Responses metadata during an active multi-step turn.

Implementation steps:

1. Define a normalized Responses output item type that can hold known fields and preserve unknown JSON fields.
2. During streaming/buffered processing, accumulate completed output items in memory.
3. Use these items for tool-call parsing and continuation within the active turn.
4. Continue persisting current message rows as the durable source of truth until a schema decision is made.
5. Add conversion helpers from output items to display text/tool calls.

Tests:

- Reasoning item with summary or encrypted content is preserved in memory.
- Tool-call linkage survives normalization.
- Unknown item fields survive round trip in the normalized item.
- Existing message history persistence remains unchanged.

Validation commands:

```text
bazel test //core:orchestrator_openai_responses_test
bazel test //core:message_parser_test
bazel test //interface:interaction_engine_test
```

Exit criteria:

- Active turn continuation can use normalized Responses items directly.
- No schema migration is required in this bundle.

### Bundle 7: Durable Responses Item Persistence Decision

Purpose: decide whether and how to preserve Responses output items across turns.

Implementation steps:

1. Evaluate whether existing `messages` rows can safely store provider output item JSON without confusing text/tool display paths.
2. If a schema change is needed, add a table such as:

   ```sql
   CREATE TABLE response_items (
     id INTEGER PRIMARY KEY,
     session_id TEXT NOT NULL,
     group_id TEXT NOT NULL,
     provider_item_id TEXT,
     item_type TEXT NOT NULL,
     status TEXT,
     item_json TEXT NOT NULL,
     created_at DATETIME DEFAULT CURRENT_TIMESTAMP
   );
   ```

3. Add write/read APIs in `Database`.
4. Keep display/query commands backwards-simple: user-visible transcript remains message-centric unless explicitly inspecting raw response items.
5. Add migration tests if schema is changed.

Tests:

- Insert/read response items.
- Preserve item order within a group.
- Missing/unknown fields rejected or preserved according to validator rules.
- Existing sessions/messages tests still pass.

Validation commands:

```text
bazel test //core:sqlite_test
bazel test //core:orchestrator_openai_responses_test
bazel test //interface:context_management_test
```

Exit criteria:

- Durable Responses item fidelity is either explicitly deferred or implemented with tested schema/API support.

### Bundle 8: PromptSections and ResponsesRequest Builder

Purpose: make request assembly and prompt-cache boundaries explicit.

Implementation steps:

1. Add `PromptSections` and `ResponsesRequest` structs.
2. Split current request assembly into:
   - section selection;
   - stable cache input construction;
   - prompt cache key computation;
   - final Responses JSON rendering.
3. Keep active skills in the dynamic tail.
4. Keep stable tools and instructions outside dynamic skill content.
5. Ensure `prompt_cache_key` only changes when stable sections change.
6. Use JSON helper utilities for parse/get/dump operations in production code.

Tests:

- Different active skills produce identical stable instructions.
- Different active skills produce identical tools.
- Different active skills produce identical history prefix.
- Different active skills produce identical `prompt_cache_key`.
- Only the dynamic skill item differs.
- Changing system instructions changes `prompt_cache_key`.

Validation commands:

```text
bazel test //core:orchestrator_openai_responses_test
bazel test //core:orchestrator_test
```

Exit criteria:

- Cache contract is visible in code and tested directly.
- Request assembly can be unit-tested without a live HTTP request.

### Bundle 9: Move DB Resolution Out of Provider Rendering

Purpose: make provider rendering pure over resolved inputs.

Implementation steps:

1. Define resolved request inputs:

   ```cpp
   struct ResolvedTurnInputs {
     std::string session_id;
     std::string system_instruction;
     std::vector<Database::Message> selected_history;
     std::vector<Database::Tool> enabled_tools;
     std::vector<Database::Skill> active_skills;
     std::string current_user_request;
   };
   ```

2. Move database reads for tools/skills/history into `Orchestrator` or a dedicated resolver before provider rendering.
3. Make `ResponsesRequestBuilder` accept `ResolvedTurnInputs`.
4. Keep tool/skill validation at the existing boundaries.
5. Remove provider-rendering database dependencies once tests cover the resolved input path.

Tests:

- Request builder tests with hand-built `ResolvedTurnInputs` and no database.
- Resolver tests with database fixtures.
- Existing request assembly tests continue to pass.

Validation commands:

```text
bazel test //core:orchestrator_openai_responses_test
bazel test //core:orchestrator_test
```

Exit criteria:

- Provider request rendering is deterministic and side-effect free.
- Database access is concentrated in resolver/orchestrator boundaries.

### Bundle 10: ContextPolicy Boundary

Purpose: isolate context selection from provider request rendering.

Implementation steps:

1. Define `ContextRequest` and `ContextWindow` structs.
2. Implement `AccordionContextPolicy` with existing behavior.
3. Inject the policy into `Orchestrator` or the resolver.
4. Add a fixed context policy for tests.
5. Keep compaction out of this bundle; this bundle only creates the boundary.

Tests:

- Accordion policy selects current epoch groups.
- Watermark reset behavior remains unchanged.
- Fixed test policy returns deterministic messages.
- Existing context management tests pass.

Validation commands:

```text
bazel test //core:orchestrator_test
bazel test //interface:context_management_test
```

Exit criteria:

- Request rendering receives selected context.
- Accordion behavior is preserved and tested through the new policy.

### Bundle 11: Optional Responses Compaction Policy

Purpose: add compaction without complicating baseline accordion selection.

Implementation steps:

1. Decide whether compaction is provider-side, local, or command-triggered.
2. Add a separate policy implementation or explicit `/responses compact` command.
3. Store compaction outputs with clear provenance.
4. Ensure compaction does not alter stable prompt prefix unexpectedly.
5. Make reset/retain behavior visible in `/context show`.

Tests:

- Compaction command/policy produces expected selected context.
- Compacted summaries do not replace original history destructively unless explicitly requested.
- Cache-key behavior is stable and documented.

Validation commands:

```text
bazel test //core:orchestrator_test
bazel test //interface:command_handler_test
bazel test //interface:context_management_test
```

Exit criteria:

- Compaction is an explicit policy/command, not hidden inside request rendering.

### Bundle 12: Collapse OrchestratorStrategy

Purpose: remove obsolete provider-generic indirection after Responses-native boundaries are in place.

Implementation steps:

1. Replace `OrchestratorStrategy` virtual calls with direct composition:

   ```text
   Orchestrator
     -> ContextPolicy
     -> TurnInputResolver
     -> ResponsesRequestBuilder
     -> ResponsesClient
     -> ResponsesEventDecoder
   ```

2. Remove runtime strategy replacement.
3. Rename generic provider methods to Responses-specific names where clearer.
4. Remove forwarding wrappers such as a generic `AssemblePayload()` if only one implementation remains.
5. Keep HTTP transport and event source injectable for tests.
6. Delete stale provider-neutral comments and docs.

Tests:

- Full core orchestrator test suite.
- Interaction engine tests.
- Command handler tests that assemble or show context.
- Build/package tests to catch stale includes.

Validation commands:

```text
bazel test //core:all
bazel test //interface:all
```

Exit criteria:

- `OrchestratorStrategy` is gone.
- Responses concepts are explicit at component boundaries.
- No provider-neutral abstraction remains unless it has more than one concrete implementation or a clear test seam role.

## Smaller Opportunistic Improvements

These can be done independently when touching nearby code.

### `/context show` Formatting

- Add thousands separators for token counts.
- Show latest cache tokens and cache percentage if available.
- Show selected group ids in the current accordion epoch.
- Show whether the next turn would reset due to watermark.

### Debug/Inspection Commands

- Add a command to print the rendered Responses request sections separately.
- Add a command to print the latest normalized Responses events or output items for the last turn.
- Ensure raw JSON inspection uses `core/json_utils.h` helpers in production code.

### Error UX

- Normalize provider error events into concise user messages.
- Preserve full raw error details behind debug output or a retrieval command.
- Distinguish context overflow, auth failure, cancellation, tool failure, and malformed provider response.

### Cancellation UX

- Show cancelled status immediately.
- Ensure partial streamed text is visually marked as cancelled if displayed.
- Avoid persisting cancelled partial output as a completed assistant message.

### Test Fixture Cleanup

- Seed Responses event decoder tests from existing orchestrator response fixtures.
- Add malformed examples from fuzz regressions as stable unit fixtures.
- Keep streaming fixtures small and deterministic.

## Cross-Cutting Implementation Rules

- Keep provider-specific response normalization in Responses-specific core code.
- Validate untrusted structured input before it reaches execution or persistence.
- Add unit tests when changing returned status, JSON fields, formatting callers depend on, token display, request assembly, or provider normalization.
- Add fuzz tests for new provider response/event decoders.
- Use `core/json_utils.h` helpers in production code for JSON parsing/access/dumping.
- Do not shell out from core production logic for deterministic computations.
- Keep each bundle independently reviewable and validated.

## Suggested Tracking Checklist

- [ ] Bundle 1: Typed Responses event decoder.
- [ ] Bundle 2: Streaming HTTP transport.
- [ ] Bundle 3: Responses streaming client integration.
- [ ] Bundle 4: Incremental InteractionEngine rendering.
- [ ] Bundle 5: Typed turn lifecycle status.
- [ ] Bundle 6: Preserve Responses output items in memory.
- [ ] Bundle 7: Durable Responses item persistence decision.
- [ ] Bundle 8: PromptSections and ResponsesRequest builder.
- [ ] Bundle 9: Move DB resolution out of provider rendering.
- [ ] Bundle 10: ContextPolicy boundary.
- [ ] Bundle 11: Optional Responses compaction policy.
- [ ] Bundle 12: Collapse OrchestratorStrategy.

## Recommended First Patch

Start with **Bundle 1: Typed Responses event decoder**. It is bounded, testable, and creates the event contract needed by streaming, lifecycle status, output-item preservation, and eventual strategy removal. It also avoids changing user-visible behavior before the new boundary is covered by tests.
