# Plan: Rename "sentinel" to "slop"

This plan outlines the steps to replace all remaining references to "sentinel" with "slop", "slop_lib", or "std_slop" as appropriate, to align with the project's name.

## Proposed Changes

### Build System (`CMakeLists.txt`)
- Rename the static library `sentinel_lib` to `slop_lib`.
- Update all `target_link_libraries` calls that reference `sentinel_lib` to use `slop_lib`.
- Update `add_executable(std_slop main.cpp)` to link against `slop_lib`.

### Source Code
#### `completion.cpp`
- Rename `sentinel_completion` function to `slop_completion`.
- Update `InitCompletion` to set `rl_attempted_completion_function = slop_completion;`.

#### `ui.cpp`
- Change the temporary file path from `/tmp/sentinel_edit.txt` to `/tmp/slop_edit.txt`.

### Tests
#### `database_test.cpp`
- Update the test query `SELECT 42 as answer, 'sentinel' as name` to use `'slop'`.
- Update the expectation `EXPECT_EQ(j[0]["name"], "sentinel")` to expect `"slop"`.

#### `tool_executor_test.cpp`
- In `ExecuteBash` test, change `echo 'sentinel'` to `echo 'slop'`.
- In `IndexAndSearch` test, change `void sentinel_function() {}` to `void slop_function() {}`.
- Update search query `search_code` from `sentinel_function` to `slop_function`.

## Verification Plan

### Automated Tests
- Run `mkdir -p build && cd build && cmake .. && make` to ensure the project still compiles after renaming.
- Run all tests:
    - `./database_test`
    - `./http_client_test`
    - `./orchestrator_test`
    - `./tool_executor_test`
    - `./command_handler_test`
    - `./completion_test`
    - `./ui_test`
- Verify that all tests pass.

### Manual Verification
- Launch `std_slop`.
- Verify tab-completion still works (if readline is available).
- Verify the editor functionality (e.g., using a command that triggers `OpenInEditor`) to ensure the new temp file path works.
- Check for any missed references using `grep -rnEi "sentinel" .`.
