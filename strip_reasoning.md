# --strip-reasoning flag (COMPLETED)

This feature allows stripping the reasoning chain from OpenAI-compatible API responses.

## Recommendation
**Recommended for:** Newer models (e.g., `o1`, `o3-mini`) when accessed via OpenRouter.
**Benefits:** Faster response times, reduced token overhead, and focused output.

## Goal
When `--strip-reasoning` is supplied to the CLI, the OpenAI-compatible API calls should include `"transforms": ["strip_reasoning"]` in the request payload. This should only apply to OpenAI-compatible models.

## Step-by-Step Execution Plan

### Phase 1: Test Infrastructure & Failing Test
1.  **Identify Test Target**: Confirm that `orchestrator_test.cpp` is the correct place to test OpenAI request generation.
2.  **Define Requirement**: The request JSON sent to the OpenAI endpoint must contain the `transforms` key with value `["strip_reasoning"]` when the feature is enabled.
3.  **Write Failing Test**: Add a test case in `orchestrator_test.cpp` that mocks the HTTP client and verifies the JSON payload.
    *   *Task ID: todo_tdd_1*
    *   *Reference*: `orchestrator_test.cpp`

### Phase 2: Configuration & Plumbing
4.  **Update Orchestrator Config**: Add `bool strip_reasoning` to the configuration struct used by `Orchestrator` and its providers.
    *   *Task ID: todo_tdd_2*
    *   *Reference*: `orchestrator.h`
5.  **Pass Flag from Main**: Update `main.cpp` to pass `absl::GetFlag(FLAGS_strip_reasoning)` into the `Orchestrator` setup logic.
    *   *Task ID: todo_tdd_3*
    *   *Reference*: `main.cpp`

### Phase 3: Implementation
6.  **Modify OpenAI Provider**: Update `orchestrator_openai.cpp` to inject the `transforms` field into the JSON body if `strip_reasoning` is enabled.
    *   *Task ID: todo_tdd_4*
    *   *Reference*: `orchestrator_openai.cpp`

### Phase 4: Verification
7.  **Run Tests**: Execute `bazel test //:orchestrator_test` and ensure the new test passes.
    *   *Task ID: todo_tdd_5*
8.  **Negative Testing**: Add/verify tests ensuring the flag has no effect on Gemini models or when set to false.
    *   *Task ID: todo_tdd_6*
9.  **Manual Smoke Test**: Run the binary with `--strip-reasoning` and verify logs (if available).
    *   *Task ID: todo_tdd_7*
