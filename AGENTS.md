do not use rg
forget backward compatibility. Do not suggest plans that are complicated by backward compatibility. We build forward.

## C++ coding style

- Write C++17 using the Google C++ Style Guide.
- Treat Abseil as the first-choice utility library. Before writing a custom helper or using a standard-library utility, check whether Abseil already provides the required behavior. Prefer `absl::StrCat`, `absl::StrSplit`, Abseil containers, `absl::Cleanup`, `absl::Mutex`, Abseil time utilities, and related primitives when they improve safety or clarity.
- Prefer existing Abseil scope and lifetime helpers over handwritten RAII wrappers. For example, use `absl::Cleanup` for scoped cleanup instead of creating a one-off cleanup class or lambda wrapper.
- Keep production C++ exception-free. Use `absl::Status` and `absl::StatusOr` for fallible operations, and propagate errors with `RETURN_IF_ERROR` and `ASSIGN_OR_RETURN`.
- Use RAII and value types by default. Do not use owning raw pointers, raw `new`, or raw `delete`; use `std::unique_ptr`, `std::shared_ptr`, or values for ownership. Raw pointers are allowed only for clearly non-owning interfaces.
- Prefer simple, readable code over complex template metaprogramming, deep inheritance, speculative abstractions, or unnecessary wrappers.
- Concurrency is allowed where it is required by the application, such as HTTP streaming, cancellation, UI responsiveness, or independent runtime work. Prefer the existing concurrency patterns and Abseil synchronization primitives. Keep ownership, cancellation, shutdown, and shared-state access explicit; avoid introducing concurrency only to save a small amount of synchronous work. Add thread-safety or TSan coverage when a change introduces new shared mutable state or synchronization behavior.
- Use `core/json_utils.h` for production JSON parsing, access, defaulting, and serialization. Use `json_parse`, `json_get`, `json_get_or`, `json_at`, and `json_dump`. Keep direct `nlohmann::json` manipulation limited to the JSON helper implementation and narrow test fixtures.
- Keep deterministic core logic in-process. Do not shell out for hashing, encoding, parsing, cleanup, or other deterministic computation.
- Add or update focused unit tests when changing behavior, status handling, validation, JSON fields, serialization, or provider normalization.
- Add or update deterministic, side-effect-free FuzzTest coverage when changing code that consumes untrusted structured input, including tool arguments, provider responses, dispatcher validation, JSON helpers, or file-content and line-range parsing.
- Fuzz tests should check the repository invariants: no crash, malformed input is rejected cleanly, invalid shapes do not reach side effects, and normalization is deterministic.
- Keep fuzz targets in the same package as the code under test. Update that package's `BUILD.bazel` with the standard FuzzTest dependencies when adding a target.
- Use both unit and fuzz tests for parser, validator, dispatcher-boundary, JSON-helper, and provider-normalization changes. Unit tests lock exact behavior; fuzz tests cover malformed and novel inputs.
- Run the affected targets and the full test suite before delivery. When validating fuzz changes, also run:
  ```sh
  bazel test //core:*fuzz_test //tools:*fuzz_test //interface:*fuzz_test
  ```
  Use `--nocache_test_results` when cache-independent fuzz validation is needed.

## JSON helpers

- In production code, use `core/json_utils.h`: `json_parse`, `json_get`, `json_get_or`, `json_dump`.
- Do not add new raw `nlohmann::json::parse(...)`, `.dump()`, or repeated manual field extraction in handlers, dispatcher code, or orchestrator code.
- Raw `nlohmann::json` access is fine inside `core/json_utils.h` and in narrow test fixture setup.

## Tool argument validation

- Treat tool arguments as untrusted input.
- Validate argument shape before side effects.
- Keep single-tool validation local to that tool.
- Move validation into `tools/common.{h,cpp}` only when the same check is used by multiple tools or when the validator itself is being tested/fuzzed.
- Do not move tool-specific semantics into `tools/common.{h,cpp}` just to share a small helper.

## Dispatcher and orchestrator boundaries

- Validate tool-call shape before dispatch/execution.
- Keep tool-call validation near the dispatcher boundary, not inside side-effecting code.
- Keep provider-specific response normalization in `core/orchestrator_*.cpp`.
- If the same provider payload shape is parsed in more than one place, centralize that parsing in the orchestrator/helper instead of duplicating it in callers or tests.

## Core logic execution rules

- Do not shell out from core production logic for hashing, encoding, parsing, or other deterministic computations.
- Implement deterministic core logic in-process; shelling out is unacceptable in `core/*.cpp` except for explicitly user-facing shell/tool features.

## When to add unit tests

- Add or update a unit test when changing success-path behavior, returned error/status, JSON field handling/defaulting, line-range behavior, path validation, argument validation, provider response normalization, or formatting/serialization that callers depend on.
- Unit tests should cover representative valid input, expected rejection/error for invalid input, boundary cases for optional/missing fields and range limits, and stable provider/tool payload shapes already used elsewhere in tests.
- Prefer unit tests for exact behavior and regressions, even if a fuzz test also exists.

## When to add fuzz tests

- Add a fuzz test when changing code that accepts untrusted structured input from tool arguments, provider responses, tool-call dispatch/validation, JSON helper parsing/access, or file content/line-range handling.
- Fuzz targets should check: no crash, malformed input is rejected cleanly, normalization returns either a usable internal form or an error, and malformed shapes do not reach execution.
- Keep fuzz harnesses deterministic and side-effect free.
- Seed from existing `*_test.cpp` payloads, `messages` rows via `query_db`, and provider response fixtures already used by orchestrator tests.

## BUILD files

- When adding a new source or test in a package, update that package's `BUILD.bazel`.
- Keep test and fuzz target deps aligned with the production code they exercise; add only the extra test libraries they actually need.
- Keep fuzz targets in the same package as the code under test.

## MCP OAuth and authentication

- Keep OAuth protocol and token-store operations (PKCE, callback verification, code exchange, refresh, and secure file writes) in `mcp/client`; CLI and gateway adapters should map their config and I/O into shared APIs instead of duplicating the flow.
- Keep gateway configuration independent from `mcp.ini`. Store only references to client-secret environment variables in config; do not accept secret values as command-line arguments or persist them in config/registry. Do not print token or secret values.
- Check stored issuer/resource bindings before token use and refresh. Preserve the existing refresh token when a provider omits a replacement.
- Use generic shared validation in `mcp/validation` when multiple MCP components need it; keep OAuth profile and transport policy in the owning config parser.
- Test OAuth login and refresh with a fake `HttpClient`; cover callback state/issuer checks, token binding and retention, token-file persistence, and secret-safe output.
