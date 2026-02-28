// ============================================================================
// SLOP JS ENVIRONMENT
// ============================================================================
// This environment provides direct access to the orchestrator state and tools.
// 
// GLOBALS:
//   session_id (string): The unique ID for the current interaction.
//   scratchpad (object): The persistent programmatic scratchpad.
//   state      (string): The technical state summary (Goal/Context/Resolved).
//   history    (array):  The full conversation history as a list of message objects.
//   tools      (object): The registry of available tools.
// ============================================================================

// Transform scratchpad from string to object if needed
if (typeof scratchpad === "string") {
  try {
    scratchpad = JSON.parse(scratchpad);
  } catch (e) {
    scratchpad = { notes: scratchpad };
  }
} else if (scratchpad === undefined || scratchpad === null) {
  scratchpad = {};
}

if (state === undefined || state === null) {
  state = "";
}

if (history === undefined || history === null) {
  history = [];
}

if (tools === undefined || tools === null) {
  tools = {};
}

// Helper to escape shell arguments
function shell_escape(s) {
  if (typeof s !== "string") s = String(s);
  return "'" + s.replace(/'/g, "'\\''") + "'";
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
let _initial_scratchpad_json = "{}";
let _initial_state = null;

core.load_session_state = function() {
  // Skip if already loaded
  if (history && history.length > 0) {
    return;
  }
  if (!session_id || session_id === "") return;
  if (_loaded_session === session_id) return;

  // Use query_db to fetch session data
  const rows_json = tools.query_db({
    sql: "SELECT scratchpad, context_size FROM sessions WHERE id = ?",
    params: [session_id]
  });
  const rows = JSON.parse(rows_json);
  let window_size = 0;
  if (rows && rows[0]) {
    const raw_sp = rows[0].scratchpad || "{}";
    try {
      scratchpad = JSON.parse(raw_sp);
    } catch (e) {
      scratchpad = {};
    }
    _initial_scratchpad_json = JSON.stringify(scratchpad);
    window_size = rows[0].context_size || 0;
  } else {
    scratchpad = {};
    _initial_scratchpad_json = "{}";
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

  // Load history
  let hist_sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY id DESC";
  if (window_size > 0) {
    hist_sql += " LIMIT " + window_size;
  }
  const hist_json = tools.query_db({
    sql: hist_sql,
    params: [session_id]
  });
  const h_rows = JSON.parse(hist_json);
  history = [];
  if (h_rows) {
    // Reverse DESC order to get chronological history
    for (let i = h_rows.length - 1; i >= 0; i--) {
      history.push({role: h_rows[i].role, content: h_rows[i].content});
    }
  }

  _loaded_session = session_id;
};

core.maybe_persist_state = function() {
  if (!session_id || session_id === "") return;
  
  const current_json = JSON.stringify(scratchpad);
  if (current_json !== _initial_scratchpad_json) {
    tools.query_db({
      sql: "INSERT INTO sessions (id, scratchpad) VALUES (?, ?) " +
            "ON CONFLICT(id) DO UPDATE SET scratchpad = excluded.scratchpad",
      params: [session_id, current_json]
    });
    _initial_scratchpad_json = current_json;
  }
  
  if (state !== _initial_state) {
    tools.query_db({
      sql: "INSERT INTO session_state (session_id, state_blob) VALUES (?, ?) ON CONFLICT(session_id) DO UPDATE SET state_blob = excluded.state_blob",
      params: [session_id, state]
    });
    _initial_state = state;
  }
};

core.wrap_result = function(name, result) {
  if (name === "manage_scratchpad") {
    if (result === null || result === "" || (typeof result === "object" && Object.keys(result).length === 0)) {
      return `### TOOL_RESULT: ${name}\nScratchpad is empty\n---`;
    }
  }
  if (typeof result === "object") {
    try {
      const res_str = JSON.stringify(result);
      return `### TOOL_RESULT: ${name}\n${res_str}\n---`;
    } catch (e) {
      let items = [];
      for (let k in result) {
        items.push(k + ": " + result[k]);
      }
      return `### TOOL_RESULT: ${name}\n${items.join("\n")}\n---`;
    }
  }
  return `### TOOL_RESULT: ${name}\n${result}\n---`;
};

const MM_TOOLS = {
  git_commit_patch: true,
  git_reroll_patch: true,
  git_verify_series: true,
  git_format_patch_series: true,
  git_finalize_series: true
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
  } catch (e) {}

  const branch = git.get_current_branch();
  if (!branch) return;

  if (!branch.startsWith("slop/staging/") && branch !== "HEAD") {
    throw new Error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " + branch);
  }
}

core.dispatch_tool = function(name, args) {
  core.load_session_state();
  
  if (MM_TOOLS[name] || MOD_TOOLS[name]) {
    slop_guard();
  }
  
  const tool_func = tools[name];
  if (!tool_func) {
    throw new Error("NOT_FOUND: Tool not found: " + name);
  }
  
  try {
    const result = tool_func(args);
    core.maybe_persist_state();
    return core.wrap_result(name, result);
  } catch (e) {
    return core.wrap_result(name, "Error: " + e.toString());
  }
};

// Git Helpers
const git = {};

git.get_current_branch = function() {
  try {
    const res = tools.execute_bash({command: "git rev-parse --abbrev-ref HEAD 2>/dev/null"});
    return res.trim();
  } catch (e) {
    return null;
  }
};

// Knowledge Management Tools
tools.manage_scratchpad = function(args) {
  if (!session_id || session_id === "") throw new Error("FAILED_PRECONDITION: No active session");
  const action = args.action;
  
  if (action === "read") {
    return scratchpad;
  } else if (action === "update") {
    if (args.key) {
      scratchpad[args.key] = args.value;
    } else if (args.content) {
      scratchpad.notes = args.content;
    } else if (typeof args.value === "object") {
      for (let k in args.value) scratchpad[k] = args.value[k];
    }
    
    const json_str = JSON.stringify(scratchpad);
    tools.query_db({
      sql: "UPDATE sessions SET scratchpad = ? WHERE id = ?",
      params: [json_str, session_id]
    });
    return "Scratchpad updated and persisted.";
  } else if (action === "append") {
    const current = scratchpad.notes || "";
    scratchpad.notes = current + (args.content || "");
    
    const json_str = JSON.stringify(scratchpad);
    tools.query_db({
      sql: "UPDATE sessions SET scratchpad = ? WHERE id = ?",
      params: [json_str, session_id]
    });
    return "Scratchpad appended and persisted.";
  } else {
    throw new Error("Unknown action: " + action);
  }
};

tools.describe_db = function(args) {
  const query = "SELECT name, sql FROM sqlite_master WHERE type='table'";
  const res = tools.query_db({sql: query});
  return res;
};

tools.help = function() {
  return `### Slop Orchestrator Help (JS) ###

#### Workflow Constraints ####
- **Code Editing & File IO:** Use tools.read_file({path}) and tools.write_file({path, content}).
- **Shell Commands:** Use tools.execute_bash({command = "..."}).

#### Globals ####
- tools: Object containing all available tools.
- scratchpad: (object) Structured persistent storage across turns.
- state: (string) Current operational context.

#### Core Tools ####
- read_file({path}): Reads file content.
- write_file({path, content}): Writes file content (guarded).
- list_directory({path, depth}): Lists files and directories.
- grep({path, pattern}): Unified search.
- execute_bash({command}): Executes a bash command (guarded).
- apply_patch({patch}): Applies a unified diff.
- manage_scratchpad({action, key, value}): Manages persistent storage.
- query_db({sql, params}): Executes a SQLite query.
- describe_db({}): Schema of local database.
- help(): Displays this help.
`;
};
