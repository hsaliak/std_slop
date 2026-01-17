# Bazel Migration Status (COMPLETED)

This document tracks the historical porting of the `std_slop` project from CMake to Bazel 8.x.

## Completed Steps

### Phase 1: Environment Setup
- Successfully transitioned from **CMake** to **Bazel 8.x** using **Bzlmod**.
- Configured `.bazelversion` to `8.0.0` (minimum baseline).
- Relaxed Bazel version requirement via `bazel_compatibility` in `MODULE.bazel`.
- Configured `.bazelrc` with `-std=c++17` and `--macos_minimum_os=10.15` (for `std::filesystem` support).
- Excluded all **CMake** artifacts via `.bazelignore`.

### Phase 2: Dependency Mapping
- Mapped Abseil, GoogleTest, and nlohmann_json via BCR modules.
- Managed `curl` via BCR.
- **SQLite3**: Built from source (amalgamation) in `BUILD.bazel` to ensure `SQLITE_ENABLE_FTS5` and `SQLITE_ENABLE_COLUMN_METADATA` are enabled, replacing the previous **CMake** FetchContent setup.

### Phase 3: Core Library Transition (`slop_lib`)
- Defined `cc_library` for `slop_lib` with explicit source and header lists.
- Transitioned from **CMake** Unity build to incremental Bazel compilation.
- Configured system prompt generation via `genrule`.

### Phase 4: Binary and Test Targets
- Defined `cc_binary` for `std_slop`.
- Defined all `cc_test` targets.
- Linked `readline` via `linkopts`.

### Phase 5: Verification & Cleanup
- Verified build: `bazel build //...` (Passed).
- Verified tests: `bazel test //...` (Passed).
- Removed `CMakeLists.txt`, `generate_coverage.sh`, and the `build/` directory.

## Usage
- Build: `bazel build //...`
- Test: `bazel test //...`
- Run: `bazel run //:std_slop -- [session_name]`