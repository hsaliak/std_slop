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
  } catch (e) { print("Error parsing JSON from DB: " + e.message); }

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
  let output = `### Slop Orchestrator Help (JS) ###

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
- persist_function({name, code, description, test_args, expected_result}): Persists a JS function.
- help(): Displays this help.
`;

  try {
    const res = tools.query_db({sql: "SELECT name, description FROM js_functions ORDER BY name"});
    if (res) {
      const rows = JSON.parse(res);
      if (rows.length > 0) {
        output += "\n#### Persistent Functions ####\n";
        for (const row of rows) {
          output += `- ${row.name}(): ${row.description || "No description provided."}\n`;
        }
      }
    }
  } catch (e) {}

  return output;
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

  if (args.start_line !== undefined && typeof args.start_line !== "number") {
    throw new Error("INVALID_ARGUMENT: 'start_line' must be an integer");
  }
  if (args.end_line !== undefined && typeof args.end_line !== "number") {
    throw new Error("INVALID_ARGUMENT: 'end_line' must be an integer");
  }
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
  
  const cmd = `find ${shell_escape(path)} -maxdepth ${depth} -mindepth 1`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Failed to list directory: " + res.stderr);
  }
  
  const lines = res.stdout.split("\n").filter(l => l.trim() !== "");
  const output = [];
  
  for (const line of lines) {
    let rel = line;
    if (line.startsWith(path)) {
      rel = line.substring(path.length);
      if (rel.startsWith("/")) rel = rel.substring(1);
    }
    
    if (rel !== "") {
      const type_check_cmd = `if [ -d ${shell_escape(line)} ]; then echo Directory; else echo File; fi`;
      const type_res = __os_run(type_check_cmd);
      if (type_res.stdout.includes("Directory")) {
        output.push(`Directory: ${rel}/`);
      } else {
        output.push(`File: ${rel}`);
      }
    }
  }
  
  return output.join("\n");
};

tools.grep = function(args) {
  const pattern = args.pattern;
  const path = args.path || ".";
  if (!pattern) throw new Error("grep requires a pattern");
  
  const cmd = `grep -rnE -e ${shell_escape(pattern)} ${shell_escape(path)}`;
  let res = "";
  try {
    res = tools.execute_bash({command: cmd});
  } catch (e) {
    if (e.message.includes("status 1")) return "";
    throw e;
  }
  
  const limit = args.limit ? parseInt(args.limit) : 500;
  const lines = res.split("\n");
  if (lines.length > limit) {
    return lines.slice(0, limit).join("\n") + "\n[TRUNCATED: Use a more specific pattern or path to narrow results]";
  }
  return res;
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

tools.git_commit_patch = function(args) {
  slop_guard();
  const summary = args.summary;
  const rationale = args.rationale;
  
  if (!summary) throw new Error("Summary is required");
  if (summary.length > 50) throw new Error("Summary must be <= 50 characters");
  
  const full_msg = summary + "\n\n" + (rationale || "");
  const cmd = `git commit -m ${shell_escape(full_msg)}`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) throw new Error("Commit failed: " + res.stdout + res.stderr);
  
  return tools.git_format_patch_series({});
};

tools.git_reroll_patch = function(args) {
  slop_guard();
  const index = parseInt(args.index, 10);
  const base_branch = git.resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.trim().split(/\s+/).filter(h => h.length > 0);
  
  if (isNaN(index) || index < 1 || index > commits.length) {
    throw new Error("Invalid patch index " + index + " (total patches: " + commits.length + ")");
  }
  
  const target_hash = commits[index - 1];
  
  const commit_res = __os_run(`git commit --fixup ${target_hash}`);
  if (commit_res.exit_code !== 0) throw new Error("Failed to create fixup commit. Are there any changes staged?");
  
  const env_cmd = `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash ${shell_escape(base_branch)}`;
  const rebase_res = __os_run(env_cmd);
  if (rebase_res.exit_code !== 0) {
    __os_run("git rebase --abort");
    throw new Error("Rebase failed. You may have conflicts. Manual intervention required.\n" + rebase_res.stderr);
  }
  
  return tools.git_format_patch_series({base_branch: base_branch});
};

tools.git_verify_series = function(args) {
  slop_guard();
  git.assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before running this command.");

  const command = args.command;
  if (!command) throw new Error("command is required");
  const base_branch = git.resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.trim().split(/\s+/).filter(h => h.length > 0);
  const current_branch = git.get_current_branch();
  const results = [];
  let all_passed = true;
  
  for (let i = 0; i < commits.length; i++) {
    const hash = commits[i];
    const co_res = __os_run(`git checkout ${hash}`);
    if (co_res.exit_code !== 0) {
      results.push({status: "failed", message: "Checkout failed", hash: hash});
      all_passed = false;
      break;
    }
    const test_res = __os_run(command);
    const status = (test_res.exit_code === 0) ? "passed" : "failed";
    results.push({status: status, hash: hash});
    if (test_res.exit_code !== 0) {
      all_passed = false;
      break;
    }
  }
  
  __os_run(`git checkout ${shell_escape(current_branch)}`);
  
  return JSON.stringify({
    all_passed: all_passed,
    report: results
  });
};

tools.git_format_patch_series = function(args) {
  slop_guard();
  const base_branch = git.resolve_base_branch(args.base_branch);
  
  const log_cmd = "git log --reverse --format='### Patch [%n/%N] ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b' " + shell_escape(base_branch) + "..HEAD";
  const log_res = tools.execute_bash({command: log_cmd});
  
  const diff_cmd = `git diff ${shell_escape(base_branch)}..HEAD`;
  const diff_res = tools.execute_bash({command: diff_cmd});
  
  return "--- MAIL SERIES ---\nBase: " + base_branch + "\n\n" + log_res + "\n\n--- FULL DIFF ---\n" + diff_res;
};

tools.git_finalize_series = function(args) {
  slop_guard();
  git.assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before finalizing.");

  const current_branch = git.get_current_branch();
  const target_branch = git.resolve_base_branch(args.target_branch);

  const hash_res = tools.execute_bash({command: "git rev-parse HEAD"});
  const hash = hash_res.trim();

  const approval_res = tools.query_db({
    sql: "SELECT approved_hash FROM patch_approvals WHERE branch_name = ?",
    params: [current_branch]
  });
  
  let approved = false;
  if (approval_res) {
    try {
      const rows = JSON.parse(approval_res);
      for (const row of rows) {
        if (row.approved_hash === hash) {
          approved = true;
          break;
        }
      }
    } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  
  if (!approved) {
    throw new Error("Patch series not approved or hash mismatch. Please obtain approval for hash " + hash + " before finalizing.");
  }

  const res1 = __os_run(`git checkout ${shell_escape(target_branch)}`);
  if (res1.exit_code !== 0) {
    throw new Error("Failed to checkout target branch '" + target_branch + "': " + res1.stderr);
  }

  const res2 = __os_run(`git merge --ff-only ${shell_escape(current_branch)}`);
  if (res2.exit_code !== 0) {
    __os_run(`git checkout ${shell_escape(current_branch)}`);
    throw new Error("Merge failed: " + res2.stderr);
  }

  __os_run(`git branch -D ${shell_escape(current_branch)}`);
  
  tools.query_db({
    sql: "DELETE FROM staging_branches WHERE branch_name = ?",
    params: [current_branch]
  });
  tools.query_db({
    sql: "DELETE FROM patch_approvals WHERE branch_name = ?",
    params: [current_branch]
  });

  return "Successfully finalized series and merged into " + target_branch;
};

git.is_staging_branch = function() {
  const branch = git.get_current_branch();
  return branch && branch.startsWith("slop/staging/");
};

tools.git_branch_staging = function(args) {
  const name = args.name;
  const base_branch = args.base_branch || git.get_current_branch();
  const staging_name = "slop/staging/" + name;
  
  const cmd = `git checkout -b ${shell_escape(staging_name)} ${shell_escape(base_branch)}`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Failed to create staging branch: " + res.stdout + res.stderr);
  }

  tools.query_db({
    sql: "INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
    params: [staging_name, base_branch]
  });
  
  return "Created and checked out staging branch: " + staging_name + " (base: " + base_branch + ")";
};

tools.grep_tool = function(args) {
  return tools.grep(args);
};

tools.use_skill = function(args) {
  if (!session_id || session_id === "") throw new Error("FAILED_PRECONDITION: No active session");
  const name = args.name;
  const action = args.action || "activate";
  
  const res = tools.query_db({
    sql: "SELECT active_skills FROM sessions WHERE id = ?",
    params: [session_id]
  });
  
  let rows = [];
  if (res) {
    try { rows = JSON.parse(res); } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  if (rows.length === 0) throw new Error("Session not found: " + session_id);
  
  let skill_list = [];
  if (rows[0].active_skills && rows[0].active_skills !== "null") {
    try { skill_list = JSON.parse(rows[0].active_skills); } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  
  let prompt_patch = "";
  if (action === "activate") {
    if (!skill_list.includes(name)) {
      skill_list.push(name);
      tools.query_db({
        sql: "UPDATE skills SET activation_count = activation_count + 1 WHERE name = ?",
        params: [name]
      });
    }
    const res_skill = tools.query_db({
      sql: "SELECT system_prompt_patch FROM skills WHERE name = ?",
      params: [name]
    });
    if (res_skill) {
      try {
        const skill_rows = JSON.parse(res_skill);
        if (skill_rows.length > 0 && skill_rows[0].system_prompt_patch) {
          prompt_patch = "\n\n" + skill_rows[0].system_prompt_patch;
        }
      } catch (e) { print("Error parsing JSON from DB: " + e.message); }
    }
  } else if (action === "deactivate") {
    skill_list = skill_list.filter(s => s !== name);
  }
  
  tools.query_db({
    sql: "UPDATE sessions SET active_skills = ? WHERE id = ?",
    params: [JSON.stringify(skill_list), session_id]
  });
  
  return "Skill '" + name + "' " + (action === "activate" ? "activated" : "deactivated") + "." + prompt_patch;
};

tools.persist_function = function(args) {
  const name = args.name;
  const code = args.code;
  const description = args.description || "";
  const test_args = args.test_args || [];
  const expected_result = args.expected_result;

  if (typeof name !== "string" || typeof code !== "string") {
    return [false, "Invalid arguments: name and code must be strings"];
  }

  let func;
  try {
    let eval_code = code;
    if (eval_code.trim().startsWith("return ")) {
      eval_code = "(function() { " + eval_code + " })()";
    }
    func = eval(eval_code);
  } catch (e) {
    return [false, "Syntax/Evaluation Error: " + e.message];
  }

  if (typeof func !== "function") {
    return [false, "Code must return a function"];
  }

  let actual_result;
  try {
    actual_result = func.apply(null, test_args);
  } catch (e) {
    return [false, "Runtime Error: " + e.message];
  }

  if (actual_result !== expected_result) {
    return [false, "Test Failed: Expected " + expected_result + ", got " + actual_result];
  }

  try {
    tools.query_db({
      sql: "INSERT OR REPLACE INTO js_functions (name, code, description) VALUES (?, ?, ?)",
      params: [name, code, description]
    });
  } catch (e) {
    return [false, "DB Error: " + e.message];
  }

  globalThis[name] = func;

  return [true, "Function persisted successfully"];
};
