
# slop RPC Testing Example

This example captures a practical two-layer validation strategy for `slop_rpc_service`.

## 1) Quick manual smoke test

### Start server with in-memory config

```bash
bazel build //rpc:slop_rpc_service
./bazel-bin/rpc/slop_rpc_service \
  --server_config=docs/impl/rpc/server_in_memory.cfg
```

### Call `RunPrompt` via grpcurl

#### Validation failure path (no LLM dependency)

```bash
grpcurl -plaintext \
  -proto rpc/slop_rpc.proto \
  -d '{"prompt":"   ","session_id":"smoke-1"}' \
  127.0.0.1:50051 slop.rpc.v1.SlopService/RunPrompt
```

Expected:
- gRPC status: `INVALID_ARGUMENT`
- response fields:
  - `success=false`
  - `errorCode="INVALID_ARGUMENT"`
  - non-empty `errorMessage`

#### Success path (requires valid model/API config)

```bash
grpcurl -plaintext \
  -proto rpc/slop_rpc.proto \
  -d '{"prompt":"Say hello","session_id":"smoke-2"}' \
  127.0.0.1:50051 slop.rpc.v1.SlopService/RunPrompt
```

Expected:
- `success=true`
- non-empty `content`
- `sessionId="smoke-2"`
- empty `errorCode` and `errorMessage`

## 2) Next automated coverage to add

Add one e2e smoke test that:
- launches `slop_rpc_service` as a subprocess
- waits for readiness
- sends a real gRPC request over transport
- asserts structured response fields
- shuts down the server process cleanly

For CI stability, start with the invalid-prompt case first. It validates startup,
transport, and handler contract without external LLM dependency.

## OpenAI config variants (server config)

`docs/impl/rpc/server_in_memory.cfg` already uses OpenAI provider. Choose one mode:

### A) OpenAI API key mode

```textproto
provider {
  provider: "openai"
  model: "gpt-5.3-codex"
  openai_api_key_env: "OPENAI_API_KEY"
  # optional:
  # openai_base_url: "https://openrouter.ai/api/v1"
  # use_responses: true
}
```

### B) OpenAI OAuth mode

```textproto
provider {
  provider: "openai"
  model: "gpt-5.3-codex"
  openai_oauth: true
}
```

Notes:
- OAuth setup is bootstrapped via:
  - `std_slop --fetch_openai_oauth_token`
  - or `std_slop --fetch_openai_oauth_device_token`
- In OAuth mode, `openai_api_key_env` is not required.
- In OAuth mode, runtime forces the OAuth base URL and ignores `openai_base_url`.