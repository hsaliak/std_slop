# OpenRouter Decision API MCP server

This stdio MCP server makes one OpenRouter Decisions API call for a named batch of independent `noul`, `choice`, and `score` questions. It does not generate prose, apply business thresholds, or execute application actions. All three question types use the same evaluation endpoint.

## Build and test

```sh
bazel build //mcp/decision_api:decision_api_server
bazel test //mcp/decision_api:config_test //mcp/decision_api:client_test //mcp/decision_api:server_test //mcp/decision_api:server_smoke_test
```

No OpenRouter API key is needed for the tests. They use a fake HTTP client or local stdio process. `//core:http_client_test` tests the single-attempt HTTP helper used by catalog discovery.

## Configure

Create a private config file, for example `~/.config/slop/decision-api.json`:

```json
{
  "apiKey": "YOUR_OPENROUTER_API_KEY",
  "model": "~typesafe/jev-latest"
}
```

Protect it before starting the service:

```sh
chmod 600 ~/.config/slop/decision-api.json
```

The endpoint defaults to `https://openrouter.ai/api/alpha/decisions`. The optional `endpoint` field overrides it; this URL must implement the OpenRouter Decisions API contract. `modelsEndpoint` defaults to `https://openrouter.ai/api/v1/models?output_modalities=decisions` only when the default evaluation endpoint is used. A custom evaluation endpoint disables catalog lookup unless its matching catalog URL is configured explicitly.

`model` is required. Its value is forwarded unchanged, including aliases and versioned IDs. The live OpenRouter catalog currently lists the alias ID `~typesafe/jev-latest`, which targets a versioned Jev model. Prefer an ID returned by `decision_models`; the service does not strip prefixes or rewrite unavailable names. The alias and available models can change upstream.

`timeoutMs` defaults to 30000 and accepts 1–60000. `apiKey` is sent only in the HTTPS Bearer header to the configured endpoint/catalog hosts. Redirects are not followed. The service does not retry evaluation calls.

## Register in std_slop

Add to `~/.config/slop/mcp.ini`:

```ini
[server.decisions]
transport=stdio
command=/absolute/path/to/bazel-bin/mcp/decision_api/decision_api_server
args_json=["--config","/home/USER/.config/slop/decision-api.json"]
enabled=true
```

The server checks that the config path is absolute, names a regular non-symlink file, and has no group/other permissions. MCP tool arguments never accept API keys or endpoint overrides.

## Tools

- **`decide`** submits `state` plus a `questions` map. Each question has an ID, `type`, and `instructions`; `choice` and `score` also require `criteria`. Optional `model` overrides the configured model. OpenRouter options are limited to `session_id`, `trace` (`generation_name`, `parent_span_id`, `span_name`, `trace_id`, `trace_name`), `user`, and the `provider` routing-preferences object. The API can be billed and receives the submitted state.
- **`decision_help`** returns local prose plus `structuredContent.schemas` (`decide` and normalized response schemas) and `structuredContent.examples` (a complete mixed-question request and matching response). Topics: `overview`, `noul`, `choice`, `score`, `request`, `response`, `configuration`, `models`, `errors`, and `examples`. It makes no HTTP call.
- **`decision_models`** reads the OpenRouter filtered model catalog. It returns the original catalog response, including IDs, canonical slugs, context/pricing metadata, and pagination fields. It makes no evaluation request.

### Question types

- **Noul:** yes/no probability. `noul` is P(yes), from 0 to 1. Optional criteria must contain exactly `true` and `false` descriptions. A value near 0.5 indicates uncertainty, not intensity.
- **Choice:** select one named option. `criteria` is an option-to-description object. Include a `none` option when there may be no match. `probabilities` and `confidence` are optional response fields.
- **Score:** rate against ordered levels, lowest first. `score` is the probability-weighted level index and can be fractional. `legend` and `probabilities` are optional.

Local resource limits are 32 questions, 255 Choice options per question, 64 Score levels, 1 MiB serialized request, 4 MiB response, and 64 JSON nesting levels. These are server limits, not OpenRouter's documented maximums.

## Example

```javascript
const models = await decisions.decision_models({});
const guide = await decisions.decision_help({topic: "noul"});
const result = await decisions.decide({
  state: {ticket: "I was charged twice for the same order."},
  model: "~typesafe/jev-latest",
  questions: {
    refund: {
      type: "noul",
      instructions: "Is the customer asking for money back?",
      criteria: {
        true: "They want at least one duplicate charge returned.",
        false: "They do not ask for money to be returned."
      }
    },
    team: {
      type: "choice",
      instructions: "Which team should handle the request?",
      criteria: {
        billing: "Charges, payments, refunds.",
        technical: "Product defects and system failures.",
        none: "Neither listed team is appropriate."
      }
    },
    urgency: {
      type: "score",
      instructions: "How urgent is the issue?",
      criteria: ["Can wait", "Needs attention soon", "Blocks purchases now"]
    }
  }
});
return {modelCount: models.structuredContent.total_count, guide: guide.structuredContent, decision: result.structuredContent};
```

Question IDs correlate returned answers; they are not instructions. Independent questions sharing evidence belong in one request. If an answer determines which evidence or options to fetch next, make a second request. Let application code apply named thresholds and fallback behavior, and calibrate them on representative, ambiguous, no-match, negated, and adversarial examples before relying on them. Confidence is not correctness or authorization.

OpenRouter documents Decisions HTTP statuses 400, 401, 402, 403, 404, 413, 429, 500, 502, 503, 524, and 529. Evaluation errors are returned as errors, not probabilities. The server does not retry evaluations by default; a retry can incur another charge.

## Security

The API key is stored in the server's config file, not in `std_slop` or the `run_js` gateway config. Do not pass secrets in `state`, instructions, or criteria: the configured provider receives those fields. Calls may incur charges. `decision_help` is local, but `decision_models` contacts OpenRouter. Model catalog descriptions are external data; treat them as untrusted content. Errors omit response bodies and authorization headers. Protect endpoint overrides and the config file as credential routing configuration.

For composition through `run_js`, configure the server executable as a downstream stdio MCP and allow only the desired tool names. The gateway's optional trace log can contain request state and answers; do not log sensitive decision inputs. See the [gateway setup guide](../gateway/README.md).
