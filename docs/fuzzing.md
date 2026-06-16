
# Fuzzing in std::slop

`std::slop` uses [FuzzTest](https://github.com/google/fuzztest) with Bazel `cc_test` targets to stress parsers, argument validators, and provider-response normalization paths that receive untrusted input.

## Why these fuzz tests exist

The fuzz suite is focused on invariants that protect agent/runtime boundaries:

- malformed structured input is rejected cleanly
- invalid shapes do not reach side-effecting execution paths
- normalization/parsing is deterministic for the same input
- no crash on arbitrary input

This complements unit tests (exact behavior/regression coverage) with adversarial input coverage.

## Current fuzz targets

### `core/`

- `//core:message_parser_fuzz_test`
  - Exercises message parsing robustness.
- `//core:openai_utils_fuzz_test`
  - Exercises OpenAI utility parsing/normalization paths.
- `//core:json_utils_fuzz_test`
  - Stresses `json_parse`, `json_get`, `json_get_or`, and dump/parse stability properties.
- `//core:tool_dispatcher_validation_fuzz_test`
  - Verifies tool-call validation behavior with arbitrary IDs/names/args and asserts invalid calls are not executed.
- `//core:orchestrator_normalization_fuzz_test`
  - Fuzzes OpenAI chat-completions, OpenAI responses, and Gemini `ProcessResponse` normalization paths.

### `tools/`

- `//tools:file_tool_args_fuzz_test`
  - Fuzzes `ValidateReadFileArgs` (path constraints, line-range constraints, determinism).
- `//tools:db_tool_args_fuzz_test`
  - Fuzzes `ValidateQueryDbArgs` (`sql` type requirements, `params` shape, determinism).
- `//tools:exec_tool_args_fuzz_test`
  - Fuzzes `ValidateExecuteBashArgs` (`command` required, timeout/type constraints, determinism).
- `//tools:edit_tool_fuzz_test`
  - Fuzzes exact text edit argument handling and atomic failure invariants.
- `//tools:dispatcher_executor_boundary_fuzz_test`
  - Fuzzes dispatcher call-shape validation and executor boundary behavior for unknown or malformed tool calls.
- `//tools:grep_tool_fuzz_test`
  - Fuzzes `grep` argument combinations and boundary handling for patterns, paths, and flags.
- `//tools:path_range_tools_fuzz_test`
  - Fuzzes read/write path validation and list-directory / read-file range boundaries.

### `interface/`

- `//interface:input_parsing_fuzz_test`
  - Fuzzes slash-command parsing/rendering invariants and command round-trip behavior.

## Running fuzz tests

Run all fuzz-test targets in the repo:

```bash
bazel test //core:*fuzz_test //tools:*fuzz_test //interface:*fuzz_test
```

Run one target locally while iterating:

```bash
bazel test //core:orchestrator_normalization_fuzz_test
```

Run with cache disabled when validating a new fuzz change:

```bash
bazel test --nocache_test_results //core:*fuzz_test //tools:*fuzz_test //interface:*fuzz_test
```

## How to add a new fuzz test

When changing code that consumes untrusted structured input (tool args, provider payloads, dispatcher validation, JSON helpers, file-content/range parsing), add or update fuzz coverage.

Use this pattern:

1. Keep harnesses deterministic and side-effect free.
2. Assert properties, not exact strings (no crash, clean rejection, deterministic status shape).
3. Keep validation at the boundary under test (e.g., tool arg validator, dispatcher validator, orchestrator normalization).
4. Seed from real payloads already present in the repo:
   - existing `*_test.cpp` fixtures
   - provider-response fixtures used by orchestrator tests
   - rows from `messages` via `query_db` when useful

## Build wiring conventions

Fuzz tests in this repository are regular `cc_test` targets with:

- `@fuzztest//fuzztest`
- `@fuzztest//fuzztest:fuzztest_gtest_main`
- `@googletest//:gtest`

and usually:

- `-Wno-unused-parameter`
- `-Wno-deprecated-declarations`

Keep fuzz targets in the same package as the production code they exercise (`core/`, `tools/`, `interface/`), and update that package's `BUILD.bazel` when adding a target.

## Relationship to unit tests

Fuzz tests do not replace unit tests.

- Use unit tests to lock exact behavior, statuses, and payload-shape expectations.
- Use fuzz tests to harden boundary code against malformed/novel input and to catch crashers.

Most parser/validator changes should include both:

- at least one targeted unit test for the expected behavior
- fuzz coverage for malformed input and invariants
