# Decision API MCP server plan

Status: implementation plan for the OpenRouter Decision API MCP. The current executable setup guide is [mcp/decision_api/README.md](../mcp/decision_api/README.md); implementation patches are developed on a staging series.

Scope: implement the OpenRouter Decisions API only. TypeSafe documentation remains comparison material, not a second backend. Forward configured model IDs and aliases unchanged; pinning is an optional deployment choice, not a server requirement. The fetched OpenRouter catalog lists alias ID `~typesafe/jev-latest`, targeting `typesafe/jev-1.13`; the bare string `jev-latest` is not that exact catalog ID.

Implementation status: all four bundles below are implemented on `slop/staging/decision-api-implementation` as separate patch commits. The plan remains here as the design record; see the [operator setup guide](../mcp/decision_api/README.md).

## Purpose and source review

Provide a stdio MCP server that lets an agent submit typed semantic judgments to a Decision API. The provider returns probabilities and selections, not generated prose or reasoning. Application code remains responsible for thresholds, authorization, arithmetic, workflow, and actions.

The following sources were retrieved with `curl` and compared on 2026-10-08. For GitHub blob links, their raw Markdown equivalents were fetched. The duplicated OpenRouter reference URL was read once.

- [OpenRouter Decisions skill](https://github.com/OpenRouterTeam/skills/blob/main/skills/openrouter-decisions/SKILL.md).
- [OpenRouter Decisions API reference](https://github.com/OpenRouterTeam/skills/blob/main/skills/openrouter-decisions/references/decisions-api.md).
- [TypeSafe skill](https://raw.githubusercontent.com/typesafe-ai/skills/main/skills/typesafe-ai/SKILL.md).
- [TypeSafe HTTP API](https://docs.typesafe.ai/api.md).

**Consistency finding:** the two OpenRouter documents agree on the core request (`model`, `state`, `questions`), the `noul`, `choice`, and `score` question types, and their probability-based answer meanings. They differ in purpose: the skill adds usage/model-selection guidance; the reference describes the HTTP contract. They document no separate HTTP endpoint for each primitive.

The TypeSafe skill and API were also read because they were requested. They describe the same three primitives, but have different required-field details and a different direct HTTP endpoint. This plan does not implement a direct TypeSafe backend. The live OpenRouter catalog returned `~typesafe/jev-latest`, whose target is `typesafe/jev-1.13`; forward that exact model ID or any configured model string unchanged. Do not strip the `~typesafe/` prefix or infer that the direct TypeSafe alias `jev-latest` is an OpenRouter ID.

OpenRouter implementation uses the current OpenAPI contract fetched from `https://openrouter.ai/openapi.json` and the filtered live catalog. The Decisions operation is `POST /api/alpha/decisions`; its documented statuses are 200, 400, 401, 402, 403, 404, 413, 429, 500, 502, 503, 524, and 529. `model`, `state`, and `questions` are required; the primitive-specific answer values are required while `probabilities`, `confidence`, and `legend` are optional where applicable. Noul criteria, if supplied, require `true` and `false`; Choice criteria are a description map; Score criteria are ordered. The schema sets no provider maximum for question count, Choice options, or Score levels, so the server uses documented local resource caps and does not claim they are OpenRouter limits. No authenticated or paid API evaluation was made.

## Upstream endpoints

| Operation | OpenRouter default |
| --- | --- |
| Evaluate any mixture of question types | `POST https://openrouter.ai/api/alpha/decisions` |
| Discover decision models | `GET https://openrouter.ai/api/v1/models?output_modalities=decisions` |

There is **no separate noul, choice, or score URL**. All use the configured evaluation endpoint and differ by each question's `type`. Independent questions can share one request. Dependent questions require a later request with the newly obtained evidence or options. Question IDs correlate answers and are not semantic instructions to the underlying model.

The OpenRouter evaluation route is outside `/api/v1`; do not derive it by appending `/decisions` to that prefix. Configure full operation URLs rather than guessing provider URL layouts.

## Configuration and registration

Proposed executable: `decision_api_server --config /absolute/path/to/decision-api.json`. Keep the provider credentials in this server's private configuration, separate from the gateway configuration and from MCP tool arguments.

Minimal OpenRouter configuration:

```json
{
  "apiKey": "REPLACE_WITH_OPENROUTER_KEY",
  "model": "~typesafe/jev-latest"
}
```

Expanded configuration showing proposed defaults:

```json
{
  "apiKey": "REPLACE_WITH_OPENROUTER_KEY",
  "endpoint": "https://openrouter.ai/api/alpha/decisions",
  "modelsEndpoint": "https://openrouter.ai/api/v1/models?output_modalities=decisions",
  "model": "~typesafe/jev-latest",
  "timeoutMs": 30000
}
```

The alias `~typesafe/jev-latest` was present in the fetched public OpenRouter catalog; its `canonical_slug` is also returned. The catalog is dynamic. Check the current endpoint's catalog for availability and keep any configured ID or alias unchanged; return unsupported-model errors rather than rewriting it. Pinning a canonical/versioned ID remains an operator choice when stable model behavior matters.

| Field | Planned contract |
| --- | --- |
| `apiKey` | Required non-empty string; reject CR/LF, NUL, and other header control bytes. Sent as `Authorization: Bearer <key>` only to configured upstream URLs. Never returned through MCP or logged. |
| `endpoint` | Full HTTPS evaluation URL, defaulting to `https://openrouter.ai/api/alpha/decisions`. Overrides must implement the OpenRouter Decisions contract. Reject fragments, userinfo, control bytes, and malformed URLs. |
| `modelsEndpoint` | Optional full HTTPS discovery URL. Use the OpenRouter catalog default only with the default evaluation URL. With a custom endpoint, discovery is disabled unless explicitly configured; do not silently send its credentials to OpenRouter. |
| `model` | Required configured default model ID or alias, such as `~typesafe/jev-latest`. `decide` may override it with a validated non-empty string. Forward names unchanged; do not impose a namespace or pinned-version format. No built-in model default. |
| `timeoutMs` | Proposed local deadline: default 30000 ms, range 1–60000 ms. Includes HTTP and any bounded retry wait. |

Reject unknown configuration fields and validate the entire configuration before network traffic. A custom endpoint must implement the OpenRouter Decisions contract, not an arbitrary HTTP API. Treat endpoint selection as operator-authorized credential disclosure: disable redirects, verify TLS, and do not infer or call another host for discovery. Protect configuration files with mode `0600`; never commit real credentials. Do not include arbitrary headers, URL overrides, or API keys in agent-supplied arguments.

Register as a normal stdio MCP server in `~/.config/slop/mcp.ini`:

```ini
[server.decisions]
transport=stdio
command=/absolute/path/to/decision_api_server
args_json=["--config","/absolute/path/to/decision-api.json"]
enabled=true
```

For composition through [run_js](../mcp/gateway/README.md), the gateway owns only the executable path, config-file argument, and authorized tool names:

```json
{
  "servers": [
    {
      "alias": "decisions",
      "transport": "stdio",
      "command": "/absolute/path/to/decision_api_server",
      "args": ["--config", "/absolute/path/to/decision-api.json"],
      "allowTools": ["decide", "decision_help", "decision_models"]
    }
  ]
}
```

The gateway does not gain a credential field. Its optional trace log may capture sensitive decision state and results; the private API key must never enter those tool payloads.

## API request and response

An OpenRouter evaluation request has this shape. The model is configurable; `~typesafe/jev-latest` is an alias returned by the fetched public OpenRouter catalog; availability can change:

```json
{
  "model": "jev-latest",
  "state": {"ticket": "I was charged twice and cannot complete checkout."},
  "model": "~typesafe/jev-latest",
  "questions": {
    "needs_refund": {
      "type": "noul",
      "instructions": "Does the ticket request or imply a refund?",
      "criteria": {
        "true": "Money should be returned for a duplicate charge.",
        "false": "The customer does not request or imply returning money."
      }
    },
    "team": {
      "type": "choice",
      "instructions": "Which team should handle the ticket?",
      "criteria": {
        "billing": "Charges, payments, refunds.",
        "technical": "Application errors and checkout failures.",
        "none": "Neither listed team is appropriate."
      }
    },
    "urgency": {
      "type": "score",
      "instructions": "How urgent is the ticket?",
      "criteria": ["Can wait", "Needs attention soon", "Currently blocking purchases"]
    }
  }
}
```

Use in-process HTTPS with `Content-Type: application/json` and Bearer authorization. To inspect a saved request manually, the OpenRouter call is:

```sh
curl --fail-with-body --silent --show-error \
  --connect-timeout 10 --max-time 30 \
  'https://openrouter.ai/api/alpha/decisions' \
  -H "Authorization: Bearer $OPENROUTER_API_KEY" \
  -H 'Content-Type: application/json' \
  --data-binary @request.json
```

For model discovery, call `GET https://openrouter.ai/api/v1/models?output_modalities=decisions` with the configured key when discovery is enabled. Present returned model identifiers and aliases as provided by the catalog; do not rewrite them. A custom evaluation endpoint disables this default catalog request unless the operator explicitly configures a matching discovery endpoint.

| Answer type | Required core result | Interpretation |
| --- | --- | --- |
| `noul` | `type: "noul"`, `noul` | Probability of yes, from 0 to 1. Near 0.5 means ambiguity, not medium intensity. No separate confidence field is promised. |
| `choice` | `type: "choice"`, `choice` | Selected input option. `probabilities` and `confidence` may be present; preserve their optionality. Include a `none` option when no match is meaningful. |
| `score` | `type: "score"`, `score` | Probability-weighted rubric index; can be fractional. The first level is index 0. Legend/distribution keys are string indices, not arbitrary numeric scale values. |

Responses contain `model`, `answers` keyed by submitted question IDs, and `usage` with token counts. The OpenRouter example also includes `id`, `provider`, and `usage.cost` in USD; preserve such metadata when present without assuming every example field is mandatory. Where provided, probabilities must be finite, lie in [0,1], and sum approximately to 1. Confidence measures distribution concentration, not correctness, authorization, or permission to act.

Normalize successful output to `structuredContent` containing `model`, `answers`, `usage`, and any available OpenRouter metadata. Preserve raw numeric judgments and structured criteria/legends; do not apply hidden thresholds or manufacture confidence. Validate answer IDs and types against the submitted questions. Missing/mismatched core answers are upstream contract failures, not default decisions. Validate choice membership and score bounds against the request. Reject a partial batch as a failed evaluation rather than silently supplying missing answers.

## MCP tools and exact proposed descriptions

Expose three tools rather than duplicating transport and schemas into per-primitive wrappers. All names and fields in this section are proposed, not existing commands.

### `decide`

Description:

> Evaluate named noul, choice, and score questions against shared state using the configured Decision API. Returns typed answers, the resolved model, and available usage metadata. Noul is probability of yes; Choice selects a defined option; Score is a probability-weighted ordered-rubric index. This may incur provider charges and does not execute actions. Use decision_help for schemas, examples, and uncertainty guidance.

Arguments: required `state` (string/object/array) and non-empty `questions` object; optional `model` override; optional `openrouterOptions` limited to `session_id`, `trace` (`generation_name`, `parent_span_id`, `span_name`, `trace_id`, `trace_name`), `user`, and the OpenRouter `provider` routing-preferences object. Reject unknown option keys and invalid shapes; do not accept arbitrary top-level passthrough JSON. Shared instruction/criterion values are strings, objects, or arrays; allow null Choice descriptions only if OpenRouter's current schema accepts them.

Each question requires `type` and `instructions`; Choice/Score require the appropriate `criteria`. A Noul's optional criteria must have the documented pair of keys under the local contract. Provide full discriminated JSON schemas, not a description saying only "any JSON." Reject unknown tool/question fields before HTTP. For the first implementation, enforce local limits of 32 questions, 255 Choice options per question, 64 Score levels, 1 MiB serialized request, 4 MiB response, and 64 JSON nesting levels; the request deadline defaults to 30000 ms (1–60000 ms). These are MCP server limits, not claimed OpenRouter limits.

Return a concise text summary plus the validated `structuredContent`. Tool annotations must describe the actual behavior: external-provider access is open-world, and evaluations may incur charges. A read-only annotation, if appropriate for evaluation without data mutation, is not a claim that the call is local, private, or free. Do not promise that retries are idempotent or free.

### `decision_help`

Description:

> Explain the configured Decision API, supported question types, request and answer schemas, batching, model selection, errors, and uncertainty handling. Returns local documentation and sanitized capabilities; it does not call the provider or expose credentials.

Arguments: optional `topic` enum (`overview`, `noul`, `choice`, `score`, `request`, `response`, `configuration`, `models`, `errors`, `examples`); default `overview`.

Return both readable guidance and machine-readable schemas/examples. Overview identifies OpenRouter as the provider, the sanitized endpoint, configured model string, discovery availability, local limits, and available topics. Help describes optional response fields accurately and must not claim that an alias is available unless the live catalog reports it. Return `decide` and response schemas from the same definitions used for tool registration and help. Add parity tests so the shared input schema cannot drift from tools/list and the documented mixed-question examples pass request/response validation. Omit API keys, headers, config paths, and URL query values from help/errors.

MCP tools/list descriptions advertise help without a separate service. Through run_js, `help("decisions", "decide")` inspects the registered MCP schema, while `await decisions.decision_help({topic: "examples"})` returns the richer usage guide. These are different operations.

### `decision_models`

Description:

> List decision-capable models from the OpenRouter catalog, with available identifiers, context limits, and pricing metadata. Helps select a verified model; it does not run an evaluation. Reports when discovery is unavailable rather than inventing model IDs.

Arguments: empty object. Call the documented OpenRouter filtered catalog URL and validate its current response schema. Preserve distinctions among ID, canonical slug, and version; do not assume every catalog entry has identical metadata. If a custom evaluation endpoint is configured without a matching catalog endpoint, return an actionable discovery-unavailable result. Never silently fall back to the default OpenRouter host.

An agent can discover help before evaluation:

```javascript
const guide = await decisions.decision_help({topic: "noul"});
const result = await decisions.decide({
  state: {ticket: "Please refund the duplicate charge."},
  questions: {
    refund: {type: "noul", instructions: "Is the customer asking for money back?"}
  }
});
return {guide: guide.structuredContent, evaluation: result.structuredContent};
```

Guidance should recommend minimal named evidence, explicit candidate/rubric meanings, independent batched questions, and calibrated application-owned thresholds. Treat untrusted state as evidence rather than instructions. Validate performance on representative, ambiguous, no-match, negated, and adversarial cases. Model changes require rechecking thresholds; structured output is not a truth guarantee.

## Architecture, failures, and security

Use the existing [MCP server library](mcp-server.md) and stdio pattern from [echo_server](echo-server.md). Proposed package: `mcp/decision_api/`, with config parsing, OpenRouter request/response helpers, an injectable HTTP client, tool registration, executable, and tests. Keep one shared transport for all primitives and OpenRouter response normalization next to the client; do not duplicate raw payload parsing in handlers or tests.

Production code is C++17 with exceptions disabled, `absl::Status`/`StatusOr`, and `core/json_utils.h`. Perform HTTPS in-process using libcurl or a suitable existing in-process abstraction after inspection; never spawn `curl` from production logic. Keep MCP stdout exclusively for protocol frames. Do not add async execution or threading merely for three tool types.

Classify argument/config errors before network access. Classify authentication, rate limit, overload, deadline, transport, malformed-response, and upstream validation failures separately. Include sanitized status and actionable guidance, never authorization headers or unrestricted upstream bodies. Error results must not resemble valid probability answers.

Inspect OpenRouter's linked OpenAPI error contract before implementing status-specific behavior. Do not assume TypeSafe's documented status codes apply to OpenRouter. Do not retry credential/schema failures. Plan bounded backoff for confirmed transient OpenRouter failures, respecting valid Retry-After and the overall deadline. Default evaluation retries to disabled until duplicate-charge/idempotency behavior is verified. Document that an operator-enabled retry may repeat a paid evaluation. No automatic endpoint fallback or silent model switch.

Bound response bodies before JSON parsing, reject non-finite or out-of-range values, and redact credentials from every diagnostic. Provide fake-HTTP tests so the suite never needs real keys or billed requests. Live probes, if needed to resolve contract discrepancies, require operator authorization and disposable non-sensitive fixtures.

## Bundle-based implementation plan

Implement the OpenRouter backend only. Keep each bundle independently reviewable; complete its validation before starting the next. No live or billable call is required by the default test suite. The current implementation uses local limits of 32 questions, 255 Choice options per question, 64 Score levels, 1 MiB serialized request, 4 MiB response, and 64 JSON nesting levels; these are server bounds, not claimed upstream maxima.

### Bundle 1: Lock the OpenRouter contract and configuration

**Implementation**
- Before coding the HTTP layer, fetch OpenRouter's current linked OpenAPI specification and filtered model-catalog response schema. Record request/response fixtures and settle currently undocumented Choice/Score limits and optional fields. The TypeSafe comparison is context only; it is not an OpenRouter schema.
- Create `mcp/decision_api/` with strict config types and parsing. Config requires a non-empty API key and model string; default `endpoint` to `https://openrouter.ai/api/alpha/decisions`, and provide the OpenRouter filtered catalog URL only when using that default endpoint.
- Treat model names as opaque non-empty strings. Accept `jev-latest` and versioned IDs, forward them unchanged, and allow the upstream/catalog to report unavailability. Pinning is an operator choice, not a parser rule.
- Permit a custom evaluation endpoint only by explicit config. If it differs from the OpenRouter default, disable catalog lookup unless the operator explicitly supplies the matching catalog URL. Never silently send the API key to an extra host.
- Add a bounded timeout setting with a documented local default/range. Reject invalid URLs, credentials with HTTP header control characters, unknown config fields, and wrong JSON types before creating clients.

**Validation**
- Unit tests cover absent/empty key and model, endpoint/catalog defaults, custom endpoint discovery-off behavior, `jev-latest` and arbitrary model-ID pass-through, malformed URL, CR/LF/NUL key rejection, timeout boundaries, wrong field types, and unknown fields.
- Add deterministic URL/config fuzz coverage; malformed configuration must fail before any fake transport request.
- Update `mcp/decision_api/BUILD.bazel` and root package references for only the sources/test targets added in this bundle.

### Bundle 2: Implement evaluation and OpenRouter response normalization

**Implementation**
- Define one validated `decide` request schema: shared `state` and a non-empty map of named questions; discriminated `noul`, `choice`, and `score` question shapes; optional model override; explicit allow-list for `session_id`, `trace`, `user`, and `provider`. Do not accept arbitrary upstream JSON passthrough.
- Implement a single in-process HTTPS POST to the configured Decisions endpoint. Use an injectable transport for tests; never shell out to curl. Send Bearer auth and JSON content type, disable redirects, bound request/response bytes, and enforce the overall deadline.
- Parse one answer per submitted question ID. Enforce core answer `type`/value shape and question correlation; preserve OpenRouter-optional `probabilities`, `confidence`, `legend`, and metadata only when present. Validate finite probability values/ranges and distribution sums within a documented tolerance; validate choice membership and score bounds. Do not manufacture missing confidence or apply business thresholds.
- Return a normalized value suitable for MCP `structuredContent` plus readable text, retaining usage/model metadata. Convert HTTP, transport, timeout, malformed JSON, and schema failures into distinct safe statuses without returning raw credential-bearing headers or unrestricted response bodies.
- Retry nothing by default. Add bounded backoff only for OpenRouter statuses proven transient by the current OpenAPI contract; respect valid `Retry-After` and total deadline. Do not retry auth/validation failures or silently switch endpoint/model.

**Validation**
- Unit tests submit all three primitives in one request; cover object/array state and instructions, omitted and present criteria, named answer IDs, aliases unchanged, optional ancillary fields, usage, and metadata.
- Fake-HTTP tests cover representative authentication, validation, rate-limit, overload, and other statuses actually listed by the verified OpenRouter schema; bad content type, invalid JSON, oversized body, missing/wrong answer IDs/types, invalid probability distributions, unknown Choice result, out-of-range Score, connection failure, redirect, cancellation, and deadline exhaustion. Assert no network call for invalid tool arguments.
- Fuzz request/response parsing from deterministic fixtures. Malformed or partial response shapes must return an error and never produce a usable completion.

### Bundle 3: Add model discovery and agent-facing help

**Implementation**
- Add `decision_models` using `GET /api/v1/models?output_modalities=decisions` at the default OpenRouter catalog endpoint. Validate its response against the fetched current schema; preserve catalog `id`, canonical slug, version, and metadata rather than synthesizing model names.
- If discovery is disabled for a custom endpoint, return a clear unsupported result; do not fall back to the default host. Do not cache model lists in the first version unless a refresh/expiry contract is designed.
- Add `decision_help` as a local-only tool. It returns overview, schemas, examples, field optionality, config/model/catalog behavior, errors, uncertainty guidance, and local limits. No key, headers, config contents, or HTTP call may appear in help.
- Use one shared schema/description source for MCP registration, validation, and help output. Publish accurate tool descriptions for `decide`, `decision_help`, and `decision_models`; mark `decide` as external and potentially billable, not local or free.

**Validation**
- Unit tests cover model catalog valid/invalid fixtures, different model ID shapes, missing optional metadata, HTTP/catalog errors, and disabled discovery.
- Assert help is deterministic and makes zero HTTP requests, accurately explains Noul/Choice/Score and optional fields, includes the exact catalog alias `~typesafe/jev-latest` without claiming permanent availability, and contains no secret or config value.
- Test MCP `tools/list` descriptions and schemas against the help schemas so they cannot drift.

### Bundle 4: Wire up the stdio server, gateway use, and operator docs

**Implementation**
- Add `decision_api_server --config <path>` using the repository MCP server library. Register exactly the three tools; keep all API work behind `decide`/`decision_models` handlers and validate tool-call shape before side effects.
- Document `~/.config/slop/mcp.ini` registration and a `run_js` gateway downstream example that passes only executable/config path and allow-listed tool names. Keep the key in the decision server's protected config, never gateway arguments, JS state, or tool calls.
- Document the OpenRouter endpoint, optional custom endpoint behavior, the filtered model catalog, curl example, complete request/result shapes, tool descriptions/help, charge/privacy warning, and troubleshooting. Keep direct TypeSafe backend instructions out of scope.

**Validation**
- Subprocess tests exercise `initialize`, `tools/list`, local help/models-disabled behavior, one successful evaluation through a local fake HTTP server, and safe error responses. Test run_js composition with the fake service; no test needs a real key or billed request.
- Check all config/request JSON examples and links. Verify secrets are absent from stdout, logs, MCP results, and errors; verify the key only reaches the configured HTTPS origin.
- Update BUILD targets and package test/fuzz targets with minimal dependencies. Build affected binaries, run focused tests, then run `bazel test //...`. Any live smoke probe is separately opt-in, explicitly authorized, non-sensitive, and excluded from normal tests.

Implementation is OpenRouter-only. The 4 MiB catalog/evaluation response cap and local input limits are enforced separately from upstream limits. Tests use fake HTTP or stdio-only local calls; no authenticated or billed evaluation was performed. Keep TypeSafe findings as comparison notes, not implementation behavior.
Implementation guidance: reuse the existing JSON helpers and HTTP client; prefer focused abstractions and existing Abseil/standard-library facilities over duplicate implementations.
