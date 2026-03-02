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

// Helper to escape shell arguments
function shell_escape(s) {
  if (typeof s !== "string") s = String(s);
  return "'" + s.replace(/'/g, "'\''") + "'";
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

function slop_guard() {
  try {
    const res = tools.query_db({sql: "SELECT mode FROM settings WHERE id = 1"});
    if (res && res.includes('"standard"')) {
      return;
    }
  } catch (e) { print("Error parsing JSON from DB: " + e.message); }

  const branch = git.get_current_branch();
  if (!branch) return;

  if (!branch.startsWith("slop/staging/") && branch !== "HEAD") {
    throw new Error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " + branch);
  }
}

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

// Git Helpers
const git = {};

git.get_current_branch = function() {
  const forced = __os_run("echo $SLOP_FORCE_BRANCH_NAME").stdout.trim();
  if (forced !== "") return forced;
  
  try {
    const res = __os_run("git rev-parse --abbrev-ref HEAD 2>/dev/null");
    if (res.exit_code === 0) {
      return res.stdout.trim();
    }
    return null;
  } catch (e) {
    return null;
  }
};



// File System Tools






git.get_base_branch = function(requested_base) {
  if (requested_base && requested_base !== "") return requested_base;
  
  const current = git.get_current_branch();
  if (!current) return "main";
  
  const res_json = tools.query_db({
    sql: "SELECT parent_branch FROM staging_branches WHERE branch_name = ?",
    params: [current]
  });
  
  if (res_json) {
    try {
      const rows = JSON.parse(res_json);
      if (rows && rows.length > 0 && rows[0].parent_branch) {
        return rows[0].parent_branch;
      }
    } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  
  if (current.startsWith("slop/staging/")) {
    throw new Error("Base branch not found in database for staging branch '" + current + "'.");
  }
  
  return "main";
};

git.resolve_base_branch = function(requested) {
  return git.get_base_branch(requested);
};

git.assert_clean_workspace = function(msg) {
  const status_res = __os_run("git status --porcelain");
  if (status_res.stdout !== "") {
    throw new Error(msg || "Working tree is dirty. Please commit, stash, or discard changes.");
  }
};






git.is_staging_branch = function() {
  const branch = git.get_current_branch();
  return branch && branch.startsWith("slop/staging/");
};















