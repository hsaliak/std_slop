#!/bin/bash

# Script to generate a coverage report using gcov and lcov

# Step 1: Clean up previous coverage data
rm -rf coverage
mkdir -p coverage

# Step 2: Build the project with coverage flags
cd /Users/kailashs/Source/std_slop/build
cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="--coverage" ..
make clean
make

# Step 3: Run the tests to generate coverage data
ctest --output-on-failure

# Step 4: Capture coverage data using lcov
lcov --directory . --capture --output-file coverage.info --ignore-errors inconsistent,unsupported,range

# Step 5: Filter out system and test files
lcov --remove coverage.info '/usr/*' '*/test/*' '*/build/_deps/*' --output-file coverage_filtered.info --ignore-errors inconsistent,unsupported,range

# Step 6: Generate HTML report
mkdir -p ../coverage
genhtml coverage_filtered.info --output-directory ../coverage --ignore-errors inconsistent,unsupported,range,missing

# Step 7: Display summary
echo "Coverage report generated in /Users/kailashs/Source/std_slop/coverage"
