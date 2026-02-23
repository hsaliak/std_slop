#!/bin/bash
set -e

# Handle running via 'bazel run'
if [[ -n "$BUILD_WORKSPACE_DIRECTORY" ]]; then
    cd "$BUILD_WORKSPACE_DIRECTORY"
fi

# Configuration
COVERAGE_DIR="coverage_report"
LCOV=$(which lcov || true)
GENHTML=$(which genhtml || true)
BAZEL="bazel"

# Ensure tools are present
if [[ -z "$LCOV" || ! -x "$LCOV" ]]; then
    echo "Error: lcov not found in PATH"
    exit 1
fi
if [[ -z "$GENHTML" || ! -x "$GENHTML" ]]; then
    echo "Error: genhtml not found in PATH"
    exit 1
fi

echo "==> Running Bazel coverage..."
# --combined_report=lcov ensures Bazel aggregates coverage data into a single file.
# We use //... to cover all targets.
$BAZEL coverage --combined_report=lcov //...

COMBINED_LCOV="bazel-out/_coverage/_coverage_report.dat"

if [[ ! -f "$COMBINED_LCOV" ]]; then
    echo "Error: Coverage report not found at $COMBINED_LCOV"
    exit 1
fi

echo "==> Filtering coverage data..."
# Filter out external/third_party and generated code.
# We ignore 'empty' and 'unused' errors which are common depending on the environment and tool version.
FINAL_LCOV="coverage_filtered.info"
"$LCOV" --ignore-errors empty,unused -r "$COMBINED_LCOV" \
    "/usr/*" \
    "external/*" \
    "*/_virtual_includes/*" \
    "*/_objs/*" \
    -o "$FINAL_LCOV"

echo "==> Generating HTML report..."
rm -rf "$COVERAGE_DIR"
"$GENHTML" --ignore-errors empty,unused "$FINAL_LCOV" --output-directory "$COVERAGE_DIR" --title "Project Coverage Report"

echo "==> Done! Report generated in $COVERAGE_DIR/index.html"
rm -f "$FINAL_LCOV"
