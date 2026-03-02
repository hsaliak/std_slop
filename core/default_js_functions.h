#pragma once

#include <string>
#include <vector>

namespace slop {

struct DefaultJsFunction {
  std::string name;
  std::string description;
  std::string json_schema;
  std::string code;
};

inline const std::vector<DefaultJsFunction>& GetDefaultJsFunctions() {
  static const std::vector<DefaultJsFunction> functions = {
    {
      "apply_patch",
      R"SLOP(Applies exact-match text patches to file content.)SLOP",
      R"SLOP({"type": "object", "properties": {"path": {"type": "string", "description": "string"}, "patches": {"type": "array", "items": {"type": "object", "properties": {"find": {"type": "string"}, "replace": {"type": "string"}}, "required": ["find", "replace"]}}}, "required": ["path", "patches"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "describe_db",
      R"SLOP(Lists SQLite schema details.)SLOP",
      R"SLOP({"type": "object", "properties": {}, "required": []})SLOP",
      R"SLOP(return function(args) {
  const query = "SELECT name, sql FROM sqlite_master WHERE type='table'";
  const res = tools.query_db({sql: query});
  return res;
};)SLOP"
    },
    {
      "execute_bash",
      R"SLOP(Executes shell command (guarded in mail mode).)SLOP",
      R"SLOP({"type": "object", "properties": {"command": {"type": "string", "description": "string"}}, "required": ["command"]})SLOP",
      R"SLOP(return function(args) {
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

  return {
    stdout: res.stdout,
    stderr: res.stderr,
    exit_code: res.exit_code,
    exitCode: res.exit_code,
    output: output,
    toString: function() { return this.output; },
    valueOf: function() { return this.output; }
  };
};)SLOP"
    },
    {
      "execute_bash_async",
      R"SLOP(Runs execute_bash asynchronously.)SLOP",
      R"SLOP({"type": "object", "properties": {"command": {"type": "string", "description": "string"}}, "required": ["command"]})SLOP",
      R"SLOP(return function(args) {
  slop_guard();
  if (!args.command) throw new Error("Usage: execute_bash_async({command = '...'})");
  return tools.dispatch_async("execute_bash", args);
};)SLOP"
    },
    {
      "git_branch_staging",
      R"SLOP(Creates and checks out a new staging branch.)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string", "description": "string"}, "base_branch": {"type": "string", "description": "string (optional)"}}, "required": ["name"]})SLOP",
      R"SLOP(return function(args) {
  const name = args.name;
  const base_branch = args.base_branch || git_get_current_branch();
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
};)SLOP"
    },
    {
      "git_commit_patch",
      R"SLOP(Commits staged changes with a summary and optional rationale.)SLOP",
      R"SLOP({"type": "object", "properties": {"summary": {"type": "string", "description": "string (<=50 characters)"}, "rationale": {"type": "string", "description": "string (optional)"}}, "required": ["summary"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "git_create_staging_branch",
      R"SLOP(Creates or reuses a staging branch (unguarded).)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string", "description": "string"}, "base_branch": {"type": "string", "description": "string (optional)"}}, "required": ["name"]})SLOP",
      R"SLOP(return function(args) {
  const name = args.name;
  const base_branch = args.base_branch || git_get_current_branch();
  const staging_name = "slop/staging/" + name;
  if (!name) throw new Error("name is required");

  let res = __os_run(`git checkout -b ${shell_escape(staging_name)} ${shell_escape(base_branch)}`);
  if (res.exit_code !== 0 && (res.stdout + res.stderr).includes('already exists')) {
    res = __os_run(`git checkout ${shell_escape(staging_name)}`);
  }
  if (res.exit_code !== 0) {
    throw new Error("Failed to create staging branch: " + res.stdout + res.stderr);
  }

  tools.query_db({
    sql: "INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
    params: [staging_name, base_branch]
  });

  return "Created and checked out staging branch: " + staging_name + " (base: " + base_branch + ")";
};)SLOP"
    },
    {
      "git_finalize_series",
      R"SLOP(Finalizes the patch series by merging the staged branch after approval.)SLOP",
      R"SLOP({"type": "object", "properties": {"target_branch": {"type": "string", "description": "string (optional)"}}, "required": []})SLOP",
      R"SLOP(return function(args) {
  slop_guard();
  git_assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before finalizing.");

  const current_branch = git_get_current_branch();
  const target_branch = git_resolve_base_branch(args.target_branch);

  const hash_res = tools.execute_bash({command: "git rev-parse HEAD"});
  const hash = hash_res.stdout.trim();

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
};)SLOP"
    },
    {
      "git_format_patch_series",
      R"SLOP(Formats the patch series as a mail-thread style summary.)SLOP",
      R"SLOP({"type": "object", "properties": {"base_branch": {"type": "string", "description": "string (optional)"}}, "required": []})SLOP",
      R"SLOP(return function(args) {
  slop_guard();
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = "git log --reverse --format='### Patch [%n/%N] ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b' " + shell_escape(base_branch) + "..HEAD";
  const log_res = tools.execute_bash({command: log_cmd});
  
  const diff_cmd = `git diff ${shell_escape(base_branch)}..HEAD`;
  const diff_res = tools.execute_bash({command: diff_cmd});
  
  return "--- MAIL SERIES ---\nBase: " + base_branch + "\n\n" + log_res.output + "\n\n--- FULL DIFF ---\n" + diff_res.output;
};)SLOP"
    },
    {
      "git_reroll_patch",
      R"SLOP(Creates a fixup and rebases to reroll the specified patch.)SLOP",
      R"SLOP({"type": "object", "properties": {"index": {"type": "string", "description": "number (1-based chunk index)"}, "base_branch": {"type": "string", "description": "string (optional)"}}, "required": ["index"]})SLOP",
      R"SLOP(return function(args) {
  slop_guard();
  const index = parseInt(args.index, 10);
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.stdout.trim().split(/\s+/).filter(h => h.length > 0);
  
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
};)SLOP"
    },
    {
      "git_verify_series",
      R"SLOP(Runs a command against every patch in the series.)SLOP",
      R"SLOP({"type": "object", "properties": {"command": {"type": "string", "description": "string"}, "base_branch": {"type": "string", "description": "string (optional)"}}, "required": ["command"]})SLOP",
      R"SLOP(return function(args) {
  slop_guard();
  git_assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before running this command.");

  const command = args.command;
  if (!command) throw new Error("command is required");
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.stdout.trim().split(/\s+/).filter(h => h.length > 0);
  const current_branch = git_get_current_branch();
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
};)SLOP"
    },
    {
      "grep",
      R"SLOP(Low-level grep helper used by grep_tool.)SLOP",
      R"SLOP({"type": "object", "properties": {"pattern": {"type": "string", "description": "string"}, "path": {"type": "string", "description": "string (optional)"}, "context": {"type": "string", "description": "number (optional)"}, "limit": {"type": "string", "description": "number (optional)"}}, "required": ["pattern"]})SLOP",
      R"SLOP(return function(args) {
  if (!args || typeof args.pattern !== "string" || args.pattern === "") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }

  const pattern = args.pattern;
  const path = args.path || ".";
  const context = args.context;
  const limit = args.limit ? parseInt(args.limit, 10) : 500;

  let context_arg = "";
  if (context !== undefined && context !== null) {
    const ctx = parseInt(context, 10);
    if (!Number.isNaN(ctx) && ctx >= 0) {
      context_arg = " -C " + ctx;
    }
  }

  const cmd = "grep -rnE" + context_arg + " -e " + shell_escape(pattern) + " " + shell_escape(path);
  let res = "";
  try {
    res = tools.execute_bash({command: cmd});
  } catch (e) {
    if (e.message.includes("status 1")) return "";
    throw e;
  }

  const lines = res.output.split("\n");
  if (lines.length > limit) {
    return lines.slice(0, limit).join("\n") +
      "\n[TRUNCATED: Use a more specific pattern or path to narrow results]";
  }
  return res.output;
};)SLOP"
    },
    {
      "grep_tool",
      R"SLOP(Searches code with grep semantics.)SLOP",
      R"SLOP({"type": "object", "properties": {"pattern": {"type": "string", "description": "string"}, "path": {"type": "string", "description": "string (optional)"}, "context": {"type": "string", "description": "number (optional)"}, "limit": {"type": "string", "description": "number (optional)"}}, "required": ["pattern"]})SLOP",
      R"SLOP(return function(args) {
  if (!args || typeof args.pattern !== "string") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }
  const simplified = {
    pattern: args.pattern,
    path: args.path || args.paths,
    context: args.context,
    limit: args.limit
  };
  return tools.grep(simplified);
};)SLOP"
    },
    {
      "help",
      R"SLOP(Returns this JSON help manifest.)SLOP",
      R"SLOP({"type": "object", "properties": {}, "required": []})SLOP",
      R"SLOP(return function(args) {
  const res = tools.query_db({sql: "SELECT name, description, json_schema FROM tools WHERE is_enabled = 1"});
  return res;
};)SLOP"
    },
    {
      "list_directory",
      R"SLOP(Lists files and directories.)SLOP",
      R"SLOP({"type": "object", "properties": {"path": {"type": "string", "description": "string (optional, default '.')"}, "depth": {"type": "string", "description": "number (optional, default 1)"}}, "required": []})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "persist_function",
      R"SLOP(Validates and persists a helper function in the JS environment.)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string", "description": "string"}, "code": {"type": "string", "description": "string"}, "description": {"type": "string", "description": "string (optional)"}, "test_args": {"type": "array", "description": "array (optional)", "items": {"type": "string"}}, "expected_result": {"type": "string", "description": "any (optional)"}}, "required": ["name", "code"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "read_file",
      R"SLOP(Reads file content with optional line range and line numbers.)SLOP",
      R"SLOP({"type": "object", "properties": {"path": {"type": "string", "description": "string"}, "start_line": {"type": "string", "description": "number (optional)"}, "end_line": {"type": "string", "description": "number (optional)"}, "line_numbers": {"type": "string", "description": "boolean (optional)"}}, "required": ["path"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "use_skill",
      R"SLOP(Activates or deactivates a named skill for the current session.)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string", "description": "string"}, "action": {"type": "string", "description": "string (optional, 'activate' or 'deactivate')"}}, "required": ["name"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "write_file",
      R"SLOP(Writes file content (guarded in mail mode).)SLOP",
      R"SLOP({"type": "object", "properties": {"path": {"type": "string", "description": "string"}, "content": {"type": "string", "description": "string"}}, "required": ["path", "content"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "persist_function",
      R"SLOP(Validates and persists a helper function in the JS environment.)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string"}, "code": {"type": "string"}, "description": {"type": "string"}, "test_args": {"type": "array", "items": {"type": "string"}}, "expected_result": {"type": "string"}}, "required": ["name", "code"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "use_skill",
      R"SLOP(Activates or deactivates a skill.)SLOP",
      R"SLOP({"type": "object", "properties": {"name": {"type": "string"}, "action": {"type": "string", "description": "activate or deactivate"}}, "required": ["name"]})SLOP",
      R"SLOP(return function(args) {
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
};)SLOP"
    },
    {
      "shell_escape",
      R"SLOP(Helper to escape shell arguments)SLOP",
      "",
      R"SLOP(return function(s) {
  if (typeof s !== "string") s = String(s);
  return "'" + s.replace(/'/g, "'\''") + "'";
};)SLOP"
    },
    {
      "slop_guard",
      R"SLOP(Guard for destructive operations)SLOP",
      "",
      R"SLOP(return function() {
  try {
    const res = tools.query_db({sql: "SELECT mode FROM settings WHERE id = 1"});
    if (res && res.includes('"standard"')) {
      return;
    }
  } catch (e) { print("Error parsing JSON from DB: " + e.message); }

  const branch = git_get_current_branch();
  if (!branch) return;

  if (!branch.startsWith("slop/staging/") && branch !== "HEAD") {
    throw new Error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " + branch);
  }
};)SLOP"
    },
    {
      "git_get_current_branch",
      R"SLOP(Gets the current git branch)SLOP",
      "",
      R"SLOP(return function() {
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
};)SLOP"
    },
    {
      "git_get_base_branch",
      R"SLOP(Gets the base branch for a staging branch)SLOP",
      "",
      R"SLOP(return function(requested_base) {
  if (requested_base && requested_base !== "") return requested_base;
  
  const current = git_get_current_branch();
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
};)SLOP"
    },
    {
      "git_resolve_base_branch",
      R"SLOP(Resolves the base branch)SLOP",
      "",
      R"SLOP(return function(requested) {
  return git_get_base_branch(requested);
};)SLOP"
    },
    {
      "git_assert_clean_workspace",
      R"SLOP(Asserts that the git workspace is clean)SLOP",
      "",
      R"SLOP(return function(msg) {
  const status_res = __os_run("git status --porcelain");
  if (status_res.stdout !== "") {
    throw new Error(msg || "Working tree is dirty. Please commit, stash, or discard changes.");
  }
};)SLOP"
    },
    {
      "git_is_staging_branch",
      R"SLOP(Checks if current branch is a staging branch)SLOP",
      "",
      R"SLOP(return function() {
  const branch = git_get_current_branch();
  return branch && branch.startsWith("slop/staging/");
};)SLOP"
    },
  };
  return functions;
}

}  // namespace slop



