#!/usr/bin/env bash
set -euo pipefail

source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
server="$(rlocation "${TEST_WORKSPACE}/mcp/local_tools/local_tools_server")"
test_script="$(rlocation "${TEST_WORKSPACE}/mcp/local_tools/workspace_subprocess_test.py")"
exec python3 "${test_script}" "${server}"
