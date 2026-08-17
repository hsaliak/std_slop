#!/usr/bin/env bash
set -euo pipefail

source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
sl="$(rlocation "${TEST_WORKSPACE}/app/sl")"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT
export HOME="${tmp_dir}/home"
mkdir -p "${HOME}"

context_json="$(${sl} --ephemeral context show --json)"
grep -F '"command":"context show"' <<<"${context_json}"
grep -F '"retain_groups":2' <<<"${context_json}"

state_db="${tmp_dir}/state.db"
"${sl}" --db "${state_db}" context set --retain_groups=3 --watermark_tokens=120 >/dev/null
context_json="$(${sl} --db "${state_db}" context show --json)"
grep -F '"retain_groups":3' <<<"${context_json}"
grep -F '"watermark_tokens":120' <<<"${context_json}"

if "${sl}" --ephemeral --schema "${tmp_dir}/missing.schema" --prompt test >/dev/null 2>"${tmp_dir}/schema.err"; then
  echo "schema without json unexpectedly succeeded" >&2
  exit 1
fi
grep -F -- '--schema requires --json' "${tmp_dir}/schema.err"

if stats_error=$("${sl}" --ephemeral --json stats unexpected); then
  echo "stats with an extra argument unexpectedly succeeded" >&2
  exit 1
fi
grep -F '"code":"INVALID_ARGUMENT"' <<<"${stats_error}"
grep -F 'usage: sl stats' <<<"${stats_error}"

if unknown_error=$("${sl}" --json --unknown-flag context show); then
  echo "unknown flag unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Unknown flag: --unknown-flag' <<<"${unknown_error}"

if unknown_json_value=$("${sl}" --json=1 --unknown-flag context show); then
  echo "unknown flag with numeric json unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Unknown flag: --unknown-flag' <<<"${unknown_json_value}"
grep -F '"ok":false' <<<"${unknown_json_value}"

mcp_json="$(${sl} --json mcp list)"
grep -F '"command":"mcp"' <<<"${mcp_json}"
grep -F '"ok":true' <<<"${mcp_json}"
mcp_json="$(${sl} mcp list --db "${tmp_dir}/ignored.db" --json)"
grep -F '"command":"mcp"' <<<"${mcp_json}"
grep -F '"ok":true' <<<"${mcp_json}"

if mcp_error=$("${sl}" mcp list --json --db); then
  echo "missing MCP global value unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Flag requires a value: --db' <<<"${mcp_error}"

if positional_mcp_error=$("${sl}" --json context mcp show --json=wat); then
  echo "positional mcp invalid flag unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Invalid boolean value: --json=wat' <<<"${positional_mcp_error}"

if typed_error=$("${sl}" --json --limit=wat stats); then
  echo "invalid integer flag unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Invalid integer value: --limit=wat' <<<"${typed_error}"
grep -F '"ok":false' <<<"${typed_error}"

if mcp_typed_error=$("${sl}" --json mcp list --ephemeral=wat); then
  echo "invalid MCP trailing global flag unexpectedly succeeded" >&2
  exit 1
fi
grep -F 'Invalid boolean value: --ephemeral=wat' <<<"${mcp_typed_error}"

mcp_help="$(${sl} mcp list --help)"
grep -F 'usage: std_slop mcp' <<<"${mcp_help}"
mcp_short_help="$(${sl} mcp list -h)"
grep -F 'usage: std_slop mcp' <<<"${mcp_short_help}"

unknown_db="${tmp_dir}/unknown.db"
if "${sl}" --db "${unknown_db}" unknown >/dev/null 2>"${tmp_dir}/unknown.err"; then
  echo "unknown command unexpectedly succeeded" >&2
  exit 1
fi
test ! -e "${unknown_db}"
