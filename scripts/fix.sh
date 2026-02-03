#!/bin/bash
set -e

# Navigate to workspace root
if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
  cd "$BUILD_WORKSPACE_DIRECTORY"
else
  cd "$(dirname "$0")/.."
fi

# Use a separate output base for linting to avoid discarding the analysis cache of the main build.
MAIN_OUTPUT_BASE=$(bazel info output_base)
LINT_OUTPUT_BASE="${MAIN_OUTPUT_BASE}-lint"

echo "=== Running clang-format fixes ==="
bazel --output_base="$LINT_OUTPUT_BASE" run //:format

echo "=== Running clang-tidy fixes ==="
./scripts/clang_tidy_fix.sh

echo "All available fixes have been applied."
