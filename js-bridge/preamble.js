// ============================================================================
// SLOP JS ENVIRONMENT
// ============================================================================
// This environment provides direct access to the orchestrator state and tools.
//
// GLOBALS:
//   session_id (string): The unique ID for the current interaction.
//   state      (string): The technical state summary (Goal/Context/Resolved).
//   tools      (object): The registry of available tools.
// ============================================================================

if (typeof state === 'undefined' || state === null) {
  state = "";
}

if (typeof tools === 'undefined' || tools === null) {
  tools = {};
}

// Helper to call tools safely
function call_tool(tool_func, args) {
  try {
    const result = tool_func(args);
    return [true, result];
  } catch (e) {
    return [false, "JS error: " + e.toString()];
  }
}

const core = {};

// Internal state tracking
let _loaded_session = null;
let _initial_state = null;

core.load_session_state = function() {
  // Skip if already loaded
  if (!session_id || session_id === "") return;
  if (_loaded_session === session_id) return;

  // Use query_db to fetch session data
  const rows_json = tools.query_db({
    sql: "SELECT context_size FROM sessions WHERE id = ?",
    params: [session_id]
  });
  const rows = JSON.parse(rows_json);
  let window_size = 0;
  if (rows && rows[0]) {
    window_size = rows[0].context_size || 0;
  }

  // Load session state
  const state_json = tools.query_db({
    sql: "SELECT state_blob FROM session_state WHERE session_id = ?",
    params: [session_id]
  });
  const s_rows = JSON.parse(state_json);
  if (s_rows && s_rows[0]) {
    state = s_rows[0].state_blob || "";
  } else {
    state = "";
  }
  _initial_state = state;

  _loaded_session = session_id;
};

core.maybe_persist_state = function() {
  if (!session_id || session_id === "") return;

  if (state !== _initial_state) {
    tools.query_db({
      sql: "INSERT INTO session_state (session_id, state_blob) VALUES (?, ?) ON CONFLICT(session_id) DO UPDATE SET state_blob = excluded.state_blob",
      params: [session_id, state]
    });
    _initial_state = state;
  }
};

const TOOL_ALIASES = Object.freeze({
  list_dir: "list_directory",
  ls: "list_directory",
  dir_list: "list_directory",
  read: "read_file",
  write: "write_file",
  shell: "execute_bash",
  run_shell: "execute_bash",
  bash: "execute_bash",
  grep: "grep_tool"
});

core.resolve_tool_name = function(name) {
  if (typeof name !== "string") {
    return "";
  }
  return TOOL_ALIASES[name] || name;
};

core.aliases_for = function(canonical_name) {
  const aliases = [];
  for (const alias in TOOL_ALIASES) {
    if (TOOL_ALIASES[alias] === canonical_name) {
      aliases.push(alias);
    }
  }
  return aliases.sort();
};

const MM_TOOLS = {
  git_commit_patch: true,
  git_reroll_patch: true,
  git_verify_series: true,
  git_format_patch_series: true,
  git_finalize_series: true,
  git_create_staging_branch: false
};

const MOD_TOOLS = {
  write_file: true,
  apply_patch: true,
  execute_bash: true
};

core.dispatch_tool = function(name, args) {
  core.load_session_state();

  const canonical_name = core.resolve_tool_name(name);

  if (MM_TOOLS[canonical_name] || MOD_TOOLS[canonical_name]) {
    slop_guard();
  }

  const tool_func = tools[canonical_name];
  if (!tool_func) {
    return JSON.stringify({
      ok: false,
      tool: canonical_name || name,
      requested_tool: name,
      alias_used: canonical_name !== name,
      error: {
        type: "NOT_FOUND",
        message: "NOT_FOUND: Tool not found: " + name
      }
    });
  }

  try {
    const result = tool_func(args);
    core.maybe_persist_state();
    return JSON.stringify({
      ok: true,
      tool: canonical_name,
      requested_tool: name,
      alias_used: canonical_name !== name,
      result: result
    });
  } catch (e) {
    return JSON.stringify({
      ok: false,
      tool: canonical_name,
      requested_tool: name,
      alias_used: canonical_name !== name,
      error: {
        type: "TOOL_ERROR",
        message: "Error: " + e.toString()
      }
    });
  }
};

