# Bazel Migration Plan

This document outlines the step-by-step process for porting the `std_slop` project from CMake to the Bazel build system.

## Phase 1: Environment Setup
1.  **Define Workspace**: Create a `MODULE.bazel` (or `WORKSPACE`) file.
2.  **External Dependencies**: Setup Bazel modules or archives for:
    *   Abseil-cpp
    *   GoogleTest
    *   nlohmann_json
3.  **System Libraries**: Define `cc_library` wrappers for system-provided `sqlite3` and `libcurl`.

## Phase 2: Dependency Mapping
1.  **Abseil Targets**: Map `absl::status`, `absl::strings`, and `absl::statusor` to their respective `@com_google_absl` targets.
2.  **JSON**: Map to `@nlohmann_json//:json`.
3.  **Third Party**: Create a `third_party/` directory to manage build rules for non-Bazel native dependencies.

## Phase 3: Core Library Transition (`slop_lib`)
1.  **Target Definition**: Create a `cc_library` for `slop_lib`.
2.  **Source Management**: Transition from the "Unity build" pattern in CMake to explicit `srcs` and `hdrs` lists in Bazel to improve build incrementality and cache hits.
3.  **Include Paths**: Configure `includes = ["."]`.

## Phase 4: Binary and Test Targets
1.  **Main Binary**: Define a `cc_binary` for `std_slop`.
2.  **Tests**: Define individual `cc_test` targets for:
    *   `database_test`
    *   `tool_executor_test`
    *   `orchestrator_test`
    *   `command_handler_test`
    *   (And others identified in CMakeLists.txt)
3.  **Test Runner**: Use `@com_google_googletest//:gtest_main` for automatic test discovery.

## Phase 5: Verification & Cleanup
1.  **Full Build**: Execute `bazel build //...`.
2.  **Test Suite**: Execute `bazel test //...`.
3.  **Refinement**: Set appropriate target visibility and remove any redundant CMake-specific artifacts.
