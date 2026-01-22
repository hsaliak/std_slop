#!/bin/bash
# Unified linting script using Bazel
# Runs clang-format and clang-tidy

set -e

echo "=== Running clang-format (check) ==="
bazel run //:format.check

echo "=== Running clang-tidy (check) ==="
bazel test //:clang_tidy_test

echo "All lint checks passed."
