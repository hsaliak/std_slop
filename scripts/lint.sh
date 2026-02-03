#!/bin/bash
# Unified linting script using Bazel
# Runs clang-format and clang-tidy

set -e

echo "=== Running clang-format (check) ==="
bazel run //:format.check || {
    echo "clang-format failed. Run ./scripts/fix.sh to fix."
    exit 1
}

echo "=== Running clang-tidy (check) ==="
bazel test //:clang_tidy_test || {
    echo "clang-tidy failed. Run ./scripts/fix.sh to fix."
    exit 1
}

echo "All lint checks passed."
