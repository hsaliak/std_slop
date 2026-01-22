#!/bin/bash
# Hermetic clang-format runner
# Usage: bazel run //:clang_format_fix

CLANG_FORMAT="external/llvm_toolchain/bin/clang-format"

if [ ! -f "$CLANG_FORMAT" ]; then
    # Bzlmod path might be different
    CLANG_FORMAT=$(find bazel-out -name clang-format -type f | head -n 1)
fi

if [ -z "$CLANG_FORMAT" ]; then
    echo "Error: clang-format not found."
    exit 1
fi

FILES=$(git ls-files | grep -E '\.(cpp|h)$')
$CLANG_FORMAT -i $FILES
