#!/bin/bash
set -e

# Navigate to workspace root
if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
  cd "$BUILD_WORKSPACE_DIRECTORY"
else
  cd "$(dirname "$0")/.."
fi

echo "=== Running clang-format fixes ==="
bazel run //:format

echo "=== Running clang-tidy fixes ==="
./scripts/clang_tidy_fix.sh

echo "All available fixes have been applied."
