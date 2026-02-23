#!/bin/bash
set -e

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
$BAZEL coverage //...

echo "==> Collecting coverage data..."
# Collect all coverage.dat files
TEMP_LCOV="coverage_all.info"
rm -f "$TEMP_LCOV"
find bazel-out/ -name "coverage.dat" -exec "$LCOV" -a {} -o "$TEMP_LCOV" \;

echo "==> Filtering coverage data..."
# Filter out external/third_party and generated code
FINAL_LCOV="coverage_filtered.info"
"$LCOV" -r "$TEMP_LCOV" \
    "/usr/*" \
    "external/*" \
    "*/_virtual_includes/*" \
    "*/_objs/*" \
    -o "$FINAL_LCOV"

echo "==> Generating HTML report..."
rm -rf "$COVERAGE_DIR"
"$GENHTML" "$FINAL_LCOV" --output-directory "$COVERAGE_DIR" --title "Project Coverage Report"

echo "==> Done! Report generated in $COVERAGE_DIR/index.html"
rm -f "$TEMP_LCOV" "$FINAL_LCOV"
