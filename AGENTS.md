do not use rg
forget backward compatibility. Do not suggest plans that are complicated by backward compatibility. We build forward.

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
