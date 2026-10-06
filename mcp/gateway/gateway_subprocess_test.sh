#!/usr/bin/env bash
set -euo pipefail

source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
gateway="$(rlocation "${TEST_WORKSPACE}/mcp/gateway/run_js_server")"
echo_server="$(rlocation "${TEST_WORKSPACE}/mcp/server/echo_server")"
test_script="$(rlocation "${TEST_WORKSPACE}/mcp/gateway/gateway_subprocess_test.py")"
exec python3 "${test_script}" "${gateway}" "${echo_server}"
