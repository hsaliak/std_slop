#!/bin/bash
# Unified linting script using Bazel
# Runs clang-format and clang-tidy

set -e

# Use a separate output base for linting to avoid discarding the analysis cache of the main build.
MAIN_OUTPUT_BASE=$(bazel info output_base)
LINT_OUTPUT_BASE="${MAIN_OUTPUT_BASE}-lint"

echo "=== Running clang-format (check) ==="
bazel --output_base="$LINT_OUTPUT_BASE" run //:format.check || {
    echo "clang-format failed. Run ./scripts/fix.sh to fix."
    exit 1
}

echo "=== Running clang-tidy (check) ==="
bazel --output_base="$LINT_OUTPUT_BASE" test //:clang_tidy_test || {
    echo "clang-tidy failed. Run ./scripts/fix.sh to fix."
    exit 1
}

echo "All lint checks passed."
