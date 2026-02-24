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
# We use --combined_report=lcov to get an aggregated report.
# We add extra instrumentation flags to ensure coverage is collected on all platforms.
$BAZEL coverage --combined_report=lcov \
    --copt=-fprofile-instr-generate --copt=-fcoverage-mapping \
    --linkopt=-fprofile-instr-generate \
    --instrumentation_filter="//core[/:]" \
    //...

COMBINED_LCOV="bazel-out/_coverage/_coverage_report.dat"

if [[ ! -f "$COMBINED_LCOV" ]]; then
    echo "Error: Coverage report not found at $COMBINED_LCOV"
    exit 1
fi

# Check if the coverage report actually contains any data
if ! grep -q "DA:" "$COMBINED_LCOV"; then
    echo "Warning: Bazel generated a coverage report, but it contains no execution data (DA records)."
    echo "This often happens on macOS with certain Bazel/LLVM versions."
    echo "Tracefile content summary:"
    grep -E "SF:|LF:|LH:" "$COMBINED_LCOV" | head -n 10
fi

echo "==> Filtering coverage data..."
# Filter out external/third_party and generated code.
# We use a broad set of ignore-errors for LCOV 2.0+ compatibility.
FINAL_LCOV="coverage_filtered.info"
"$LCOV" --ignore-errors empty,unused,inconsistent,negative -r "$COMBINED_LCOV" \
    "/usr/*" \
    "external/*" \
    "*/_virtual_includes/*" \
    "*/_objs/*" \
    -o "$FINAL_LCOV"

# If the filtered file is empty or missing data, genhtml will fail.
if [[ ! -s "$FINAL_LCOV" ]] || ! grep -q "SF:" "$FINAL_LCOV"; then
    echo "Error: Filtered coverage data is empty. Cannot generate HTML report."
    echo "Possible reasons:"
    echo "  1. No tests were run or all tests failed."
    echo "  2. Instrumentation filter did not match any source files."
    echo "  3. Platform-specific coverage collection issues (e.g. macOS LLVM)."
    rm -f "$FINAL_LCOV"
    exit 1
fi

echo "==> Generating HTML report..."
rm -rf "$COVERAGE_DIR"
"$GENHTML" --ignore-errors empty,unused,inconsistent,negative "$FINAL_LCOV" --output-directory "$COVERAGE_DIR" --title "Project Coverage Report"

echo "==> Done! Report generated in $COVERAGE_DIR/index.html"
rm -f "$FINAL_LCOV"
