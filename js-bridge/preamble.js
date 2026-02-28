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
} else if (typeof scratchpad === 'undefined' || scratchpad === null) {
  scratchpad = {};
}

if (typeof state === 'undefined' || state === null) {
  state = "";
}

if (typeof history === 'undefined' || history === null) {
  history = [];
}

if (typeof tools === 'undefined' || tools === null) {
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

// File System Tools
tools.read_file = function(args) {
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  let cmd = "cat " + shell_escape(path);
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Could not open file: " + res.stderr);
  }

  let lines = res.stdout.split("\n");
  if (res.stdout.endsWith("\n")) lines.pop();

  const start_line = args.start_line || 1;
  const end_line = args.end_line || lines.length;

  if (start_line > end_line) {
    throw new Error("INVALID_ARGUMENT: start_line must be less than or equal to end_line");
  }

  let result_lines = lines.slice(start_line - 1, end_line);
  if (args.line_numbers) {
    result_lines = result_lines.map((line, i) => (start_line + i) + ": " + line);
  }

  let body = result_lines.join("\n");
  if (body.length > 0) body += "\n";
  return body;
};

tools.write_file = function(args) {
  slop_guard();
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  if (typeof args.content !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: content");

  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  // Use a temporary file and mv to be safer, or just use printf/redirect
  // For simplicity, we'll use a heredoc-like approach with base64 to avoid escaping issues
  // But wait, we don't have base64 easily. Let's just use a simple redirect for now.
  // Actually, we can use a temporary file.
  const tmp_file = ".tmp_write_" + Math.random().toString(36).substring(7);
  
  // We'll use a more robust way in the future.
  const res = __os_run("cat > " + shell_escape(path) + " << 'EOF_SLOP'\n" + args.content + "\nEOF_SLOP\n");
  
  if (res.exit_code !== 0) {
    throw new Error("IO_ERROR: Failed to write to file: " + res.stderr);
  }

  return "File written successfully:\nPath: " + path + "\nBytes written: " + args.content.length + "\n";
};

tools.execute_bash = function(args) {
  slop_guard();
  if (!args.command) throw new Error("command is required");

  const res = __os_run(args.command);
  let output = res.stdout;
  if (res.stderr !== "") {
    if (output !== "" && !output.endsWith("\n")) output += "\n";
    output += "### STDERR\n" + res.stderr;
  }

  if (res.exit_code !== 0) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + ": " + output);
  }

  return output;
};

tools.execute_bash_async = function(args) {
  slop_guard();
  if (!args.command) throw new Error("Usage: execute_bash_async({command = '...'})");
  return tools.dispatch_async("execute_bash", args);
};

tools.list_directory = function(args) {
  const path = args.path || ".";
  const depth = args.depth || 1;
  
  // Simplified version for now
  const cmd = "find " + shell_escape(path) + " -maxdepth " + depth + " -mindepth 1";
  const res = tools.execute_bash({command: cmd});
  return res;
};

tools.grep = function(args) {
  const pattern = args.pattern;
  const path = args.path || ".";
  if (!pattern) throw new Error("grep requires a pattern");
  
  const cmd = "grep -rnE " + shell_escape(pattern) + " " + shell_escape(path);
  try {
    return tools.execute_bash({command: cmd});
  } catch (e) {
    if (e.message.includes("status 1")) return "";
    throw e;
  }
};

tools.apply_patch = function(args) {
  slop_guard();
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  if (!Array.isArray(args.patches)) throw new Error("INVALID_ARGUMENT: Missing mandatory field: patches");

  const path = args.path;
  let content = tools.read_file({path: path});

  for (const patch of args.patches) {
    const find = patch.find;
    const replace = patch.replace;
    
    const idx = content.indexOf(find);
    if (idx === -1) {
      throw new Error("NOT_FOUND: Could not find exact match for the 'find' block in: " + path);
    }
    
    if (content.indexOf(find, idx + 1) !== -1) {
      throw new Error("FAILED_PRECONDITION: Multiple matches found for the 'find' block.");
    }
    
    content = content.substring(0, idx) + replace + content.substring(idx + find.length);
  }

  tools.write_file({path: path, content: content});
  return "File patched successfully: " + path;
};
