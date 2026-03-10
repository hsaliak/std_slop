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
    let result = tool_func(args);
    if (typeof result === "object" && result !== null) {
      if (result.stdout !== undefined || result.stderr !== undefined) {
        let out = result.stdout || "";
        if (result.stderr && result.stderr.trim() !== "") {
          out += "\n### STDERR\n" + result.stderr;
        }
        
        result = out;
      }
    }
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
    const [success, result] = call_tool(tool_func, args);
    if (!success) throw new Error(result);
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
        message: "Error: " + (e instanceof Error ? e.message : e.toString())
      }
    });
  }
};






// Git helper object for backward compatibility
const git = {
  get get_current_branch() { return globalThis.git_get_current_branch; },
  get get_base_branch() { return globalThis.git_get_base_branch; },
  get resolve_base_branch() { return globalThis.git_resolve_base_branch; },
  get assert_clean_workspace() { return globalThis.git_assert_clean_workspace; },
  get is_staging_branch() { return globalThis.git_is_staging_branch; }
};

// Native slop-guard bridge.
// Canonical policy now lives in C++ guarded handlers (execute_bash/write_file/
// patch_tool and git mail-model operations). Keep a compatibility symbol for
// legacy JS helper scripts that still call `slop_guard()`.
if (typeof globalThis.slop_guard !== "function") {
  globalThis.slop_guard = function() {
    return "";
  };
}

const _original_ask_user = tools.ask_user;
if (_original_ask_user) {
  tools.ask_user = function(args) {
    try {
      const res = tools.query_db({ sql: "SELECT is_enabled FROM tools WHERE name = 'ask_user'" });
      const rows = JSON.parse(res);
      if (rows.length === 0 || rows[0].is_enabled !== 1) {
        return "Error: ask_user tool is not enabled in this context.";
      }
    } catch (e) {}
    return _original_ask_user(args);
  };
}









