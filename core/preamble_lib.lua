-- Slop Lua Preamble Library
-- This file contains the implementation of Lua-based tools and helpers.

-- Helper to escape shell arguments
function shell_escape(s)
  if type(s) ~= "string" then s = tostring(s) end
  return "'" .. string.gsub(s, "'", "'\\''") .. "'"
end

-- Helper to call tools safely and handle the (pcall_ok, tool_ok, result) return pattern
function call_tool(tool_func, args)
  local results = { pcall(tool_func, args) }
  local ok = results[1]
  if not ok then
    return false, "Lua error: " .. tostring(results[2])
  end
  
  -- C++ tools return a single value on success and throw on error.
  -- Lua tools (in this preamble) return (success, result).
  -- Note: if results[2] is nil, #results will be 1.
  if #results <= 2 then
    return true, results[2]
  end
  return results[2], results[3]
end

tools = tools or {}
core = core or {}

-- Internal state tracking
local _loaded_session = nil
local _initial_scratchpad = nil
local _initial_state = nil

function core.load_session_state()
  if not session_id or session_id == "" then return end
  if _loaded_session == session_id then return end

  -- Use query_db to fetch session data
  local rows_json = tools.query_db({
    sql = "SELECT scratchpad, context_size FROM sessions WHERE id = ?",
    params = {session_id}
  })
  local rows = JSON.parse(rows_json)
  local window_size = 0
  if rows and rows[1] then
    scratchpad = rows[1].scratchpad or ""
    _initial_scratchpad = scratchpad
    window_size = rows[1].context_size or 0
  else
    scratchpad = ""
    _initial_scratchpad = ""
  end

  -- Load session state
  local state_json = tools.query_db({
    sql = "SELECT state_blob FROM session_state WHERE session_id = ?",
    params = {session_id}
  })
  local s_rows = JSON.parse(state_json)
  if s_rows and s_rows[1] then
    state = s_rows[1].state_blob or ""
  else
    state = ""
  end
  _initial_state = state

  -- Load history
  local hist_sql = "SELECT role, content FROM messages WHERE session_id = ? ORDER BY id DESC"
  if window_size > 0 then
    hist_sql = hist_sql .. " LIMIT " .. window_size
  end
  local hist_json = tools.query_db({
    sql = hist_sql,
    params = {session_id}
  })
  local h_rows = JSON.parse(hist_json)
  history = {}
  if h_rows then
    -- Reverse DESC order to get chronological history
    for i = #h_rows, 1, -1 do
      table.insert(history, {role = h_rows[i].role, content = h_rows[i].content})
    end
  end

  _loaded_session = session_id
end

function core.maybe_persist_state()
  if not session_id or session_id == "" then return end
  
  if scratchpad ~= _initial_scratchpad then
    tools.query_db({
      sql = "INSERT INTO sessions (id, scratchpad) VALUES (?, ?) ON CONFLICT(id) DO UPDATE SET scratchpad = excluded.scratchpad",
      params = {session_id, scratchpad}
    })
    _initial_scratchpad = scratchpad
  end
  
  if state ~= _initial_state then
    tools.query_db({
      sql = "INSERT INTO session_state (session_id, state_blob) VALUES (?, ?) ON CONFLICT(session_id) DO UPDATE SET state_blob = excluded.state_blob",
      params = {session_id, state}
    })
    _initial_state = state
  end
end

function core.wrap_result(name, result)
  return string.format("### TOOL_RESULT: %s\n%s\n---", name, tostring(result))
end

local function is_mail_model_tool(name)
  local MM_TOOLS = {
    git_commit_patch = true,
    git_reroll_patch = true,
    git_verify_series = true,
    git_format_patch_series = true,
    git_finalize_series = true
  }
  return MM_TOOLS[name] == true
end

local function is_base_modification_tool(name)
  local MOD_TOOLS = {
    write_file = true,
    apply_patch = true,
    execute_bash = true
  }
  return MOD_TOOLS[name] == true
end

local function slop_guard()
  if os.getenv("SLOP_SKIP_STAGING_CHECK") == "1" then return end

  local branch = git.get_current_branch()
  if not branch then return end -- Allow if not in a git repo (e.g. during unit tests)

  if not branch:find("^slop/staging/") then
    error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " .. branch)
  end
end

function core.dispatch_tool(name, args)
  -- 1. Lazy load state
  core.load_session_state()
  
  -- 2. Staging branch check for destructive tools
  if is_mail_model_tool(name) or is_base_modification_tool(name) then
    local ok, err = pcall(slop_guard)
    if not ok then
      -- Re-throw as hard error to bypass result wrapping and return raw status
      error("FAILED_PRECONDITION: Mail Model Violation: " .. tostring(err), 0)
    end
  end
  
  -- 3. Execute tool
  local tool_func = tools[name]
  if not tool_func then
    error("NOT_FOUND: Tool not found: " .. name, 0)
  end
  
  local status, result = pcall(tool_func, args)
  
  -- 4. Persist state if changed
  core.maybe_persist_state()
  
  -- 5. Wrap and return
  if not status then
    local err = tostring(result)
    if err:find("NOT_FOUND:") or err:find("FAILED_PRECONDITION:") or err:find("INVALID_ARGUMENT:") then
      error(err, 0)
    end
    return core.wrap_result(name, "Error: " .. err)
  end
  return core.wrap_result(name, result)
end

-- Git Helpers
git = git or {}

function git.get_current_branch()
  local forced = os.getenv("SLOP_FORCE_BRANCH_NAME")
  if forced and forced ~= "" then return forced end
  
  local success, branch = call_tool(tools.execute_bash, {command = "git rev-parse --abbrev-ref HEAD 2>/dev/null"})
  if not success then return nil end
  return branch:gsub("%s+", "")
end

function git.is_staging_branch()
  local branch = git.get_current_branch()
  return branch and branch:find("^slop/staging/") ~= nil
end


-- Foundation Tools (Migrated from C++)

function tools.read_file(args)
  local path = args.path
  if not path then error("path is required") end

  local f = io.open(path, "r")
  if not f then error("Could not open file: " .. path) end

  local lines = {}
  for line in f:lines() do
    table.insert(lines, line)
  end
  f:close()

  local start_line_req = args.start_line
  local end_line_req = args.end_line

  if start_line_req and end_line_req and start_line_req > end_line_req then
    error("INVALID_ARGUMENT: start_line must be less than or equal to end_line", 0)
  end

  local start_line = start_line_req or 1
  local end_line = end_line_req or #lines

  local add_line_numbers = true
  if args.add_line_numbers ~= nil then add_line_numbers = args.add_line_numbers end
  if args.line_numbers ~= nil then add_line_numbers = args.line_numbers end

  -- Validation logic similar to C++
  if start_line < 1 then start_line = 1 end
  if end_line > #lines then end_line = #lines end
  if start_line > #lines then
    return string.format("### FILE: %s | TOTAL_LINES: %d\n(requested start_line %d is beyond file length %d)\n", path, #lines, start_line, #lines)
  end

  local header = string.format("### FILE: %s | TOTAL_LINES: %d | RANGE: %d-%d\n", path, #lines, start_line, end_line)

  local body_lines = {}
  for i = start_line, end_line do
    local line = lines[i]
    if add_line_numbers then
      table.insert(body_lines, string.format("%d: %s", i, line))
    else
      table.insert(body_lines, line)
    end
  end

  local body = table.concat(body_lines, "\n")
  if #body > 0 then body = body .. "\n" end

  if end_line < #lines then
    body = body .. string.format("\n... [Truncated. Use 'read_file' with start_line=%d to see more] ...", end_line + 1)
  end

  return header .. body
end

function tools.write_file(args)
  slop_guard() -- Require staging branch for writing
  local path = args.path
  local content = args.content
  if not path then error("path is required") end
  if not content then error("content is required") end

  local f = io.open(path, "w")
  if not f then error("Could not open file for writing: " .. path) end
  f:write(content)
  f:close()

  local result = "File written successfully:\n"
  result = result .. "Path: " .. path .. "\n"
  result = result .. "Bytes written: " .. #content .. "\n"
  return result
end

function tools.execute_bash(args)
  local command = args.command
  if not command then error("command is required") end

  local res = __os_run(command)
  local output = res.stdout
  if res.stderr ~= "" then
    if output ~= "" and output:sub(-1) ~= "\n" then output = output .. "\n" end
    output = output .. "### STDERR\n" .. res.stderr
  end

  if res.exit_code ~= 0 then
    error(string.format("INTERNAL: Command failed with status %d: %s", res.exit_code, output), 0)
  end

  return output
end

-- Knowledge Management Tools

function tools.save_memo(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session") end
  local content = args.content
  local tags = args.tags or {}
  local tags_json = JSON.stringify(tags)
  
  local query = string.format("INSERT INTO llm_memos (content, semantic_tags) VALUES (%s, %s)", 
                              shell_escape(content), shell_escape(tags_json))
  local success, res = call_tool(tools.query_db, {sql = query})
  if not success then
    error("Failed to save memo: " .. tostring(res))
  end
  return "Memo saved."
end

function tools.retrieve_memos(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session") end
  local tags = args.tags or {}
  local query
  if #tags == 0 then
    query = "SELECT content, semantic_tags as tags, created_at FROM llm_memos ORDER BY created_at DESC LIMIT 20"
  else
    local tag_conditions = {}
    for _, tag in ipairs(tags) do
      table.insert(tag_conditions, string.format("semantic_tags LIKE '%%%s%%'", tag))
    end
    query = "SELECT content, semantic_tags as tags, created_at FROM llm_memos WHERE " .. table.concat(tag_conditions, " AND ") .. " ORDER BY created_at DESC"
  end
  
  local success, res = call_tool(tools.query_db, {sql = query})
  if not success then
    error("Failed to retrieve memos: " .. tostring(res))
  end
  return res
end

function tools.manage_scratchpad(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session", 0) end
  local action = args.action
  local content = args.content or ""
  
  if action == "read" then
    if not scratchpad or scratchpad == "" then
      return "Scratchpad is empty."
    end
    return scratchpad
  elseif action == "update" then
    scratchpad = content
    return "Scratchpad updated."
  elseif action == "append" then
    if not scratchpad or scratchpad == "" then
      scratchpad = content
    else
      scratchpad = scratchpad .. "\n" .. content
    end
    return "Content appended to scratchpad."
  else
    error("Unknown action: " .. tostring(action))
  end
end

-- Meta & Discovery Tools

function tools.list_directory(args)
  local path = args.path or "."
  local depth = args.depth or 1
  local git_only = args.git_only
  
  if git_only then
    local success_check, is_git = call_tool(tools.execute_bash, {command = "git rev-parse --is-inside-work-tree 2>/dev/null"})
    if success_check and is_git:find("true") then
       local cmd = "git ls-files --cached --others --exclude-standard"
       if path ~= "." then cmd = cmd .. " " .. shell_escape(path) end
       local success_git, res_git = call_tool(tools.execute_bash, {command = cmd})
       if success_git then return res_git end
    end
  end

  local cmd = string.format("find %s -maxdepth %d -mindepth 1", shell_escape(path), depth)
  local success, res = call_tool(tools.execute_bash, {command = cmd})
  if not success then error("Failed to list directory: " .. tostring(res)) end
  
  local output = {}
  for line in res:gmatch("[^\r\n]+") do
    local rel = line
    if line:sub(1, #path) == path then
      rel = line:sub(#path + 1)
      if rel:sub(1, 1) == "/" then rel = rel:sub(2) end
    end
    
    if rel ~= "" then
      local type_check_cmd = string.format("if [ -d %s ]; then echo Directory; else echo File; fi", shell_escape(line))
      local _, type_res = call_tool(tools.execute_bash, {command = type_check_cmd})
      if type_res:find("Directory") then
        table.insert(output, "Directory: " .. rel .. "/")
      else
        table.insert(output, "File: " .. rel)
      end
    end
  end
  return table.concat(output, "\n")
end

function tools.describe_db(args)
  local query = "SELECT name, sql FROM sqlite_master WHERE type='table'"
  local success, res = call_tool(tools.query_db, {sql = query})
  if not success then error("Failed to describe database: " .. tostring(res)) end
  return res
end

function tools.use_skill(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session") end
  local name = args.name
  local action = args.action or "activate"
  
  -- 1. Fetch current active_skills from DB
  local q1 = string.format("SELECT active_skills FROM sessions WHERE id = %s", shell_escape(session_id))
  local ok, res = call_tool(tools.query_db, {sql = q1})
  if not ok then error("Failed to fetch skills: " .. tostring(res)) end
  
  local rows = JSON.parse(res)
  if #rows == 0 then error("Session not found: " .. session_id) end
  
  local skill_list_json = rows[1].active_skills
  local skill_list = {}
  if skill_list_json and skill_list_json ~= "" and skill_list_json ~= "null" then
    skill_list = JSON.parse(skill_list_json)
  end

  local skill_map = {}
  for _, s in ipairs(skill_list) do skill_map[s] = true end

  local prompt_patch = ""

  if action == "activate" then
    if not skill_map[name] then
      table.insert(skill_list, name)
      -- Increment activation count in meta-table
      call_tool(tools.query_db, {sql = string.format(
        "UPDATE skills SET activation_count = activation_count + 1 WHERE name = %s", 
        shell_escape(name))})
    end
    -- Get system prompt patch
    local q_skill = string.format("SELECT system_prompt_patch FROM skills WHERE name = %s", shell_escape(name))
    local ok_skill, res_skill = call_tool(tools.query_db, {sql = q_skill})
    if ok_skill then
      local skill_rows = JSON.parse(res_skill)
      if #skill_rows > 0 then
        prompt_patch = "\n\n" .. (skill_rows[1].system_prompt_patch or "")
      end
    end
  elseif action == "deactivate" then
    local new_list = {}
    for _, s in ipairs(skill_list) do
      if s ~= name then table.insert(new_list, s) end
    end
    skill_list = new_list
  end

  -- 2. Persist back to session
  local q2 = string.format("UPDATE sessions SET active_skills = %s WHERE id = %s", 
                           shell_escape(JSON.stringify(skill_list)), shell_escape(session_id))
  local ok2, res2 = call_tool(tools.query_db, {sql = q2})
  if not ok2 then error("Failed to update active skills: " .. tostring(res2)) end

  return "Skill '" .. name .. "' " .. (action == "activate" and "activated" or "deactivated") .. "." .. prompt_patch
end

-- Search Tools

function tools.git_grep_tool(args)
  local cmd = "git grep --line-number -I"
  if args.context and args.context > 0 then cmd = cmd .. " -C " .. args.context end
  if args.before and args.before > 0 then cmd = cmd .. " -B " .. args.before end
  if args.after and args.after > 0 then cmd = cmd .. " -A " .. args.after end
  if args.case_insensitive then cmd = cmd .. " -i" end
  if args.word_regexp then cmd = cmd .. " -w" end
  if args.files_with_matches then cmd = cmd .. " -l" end
  if args.count then cmd = cmd .. " -c" end
  if args.show_function then cmd = cmd .. " -p" end
  
  if args.branch then cmd = cmd .. " " .. shell_escape(args.branch) end

  local patterns = args.patterns or {}
  if args.pattern then table.insert(patterns, args.pattern) end
  if #patterns == 0 then
    error("git_grep_tool requires at least one pattern.")
  end
  for _, p in ipairs(patterns) do
    cmd = cmd .. " -e " .. shell_escape(p)
  end
  
  local paths = args.path or {"."}
  if type(paths) == "string" then paths = {paths} end
  cmd = cmd .. " --"
  for _, p in ipairs(paths) do
    cmd = cmd .. " " .. shell_escape(p)
  end
  
  local success, res = call_tool(tools.execute_bash, {command = cmd})
  if not success then
    if res:find("status 1") then
      res = ""
    else
      error(res)
    end
  end
  
  local lines = {}
  local count = 0
  for line in res:gmatch("[^\r\n]+") do
    count = count + 1
    if count <= 500 then
      table.insert(lines, line)
    end
  end
  
  local output = table.concat(lines, "\n")
  if count > 500 then
    output = output .. "\n[TRUNCATED: Use a more specific pattern or path to narrow results]"
  end
  
  return output
end

function tools.grep_tool(args)
  local pattern = args.pattern
  local path = args.path or "."
  
  local git_check_ok, git_check_res = call_tool(tools.execute_bash, {command = "git rev-parse --is-inside-work-tree"})
  local is_git = git_check_ok and git_check_res:find("true")
  
  if is_git then
    local ok, res = pcall(tools.git_grep_tool, {pattern = pattern, path = {path}, context = args.context})
    if ok and res and res ~= "" then
      return res
    end
  end
  
  local cmd = "grep -rnE"
  if args.context and args.context > 0 then cmd = cmd .. " -C " .. args.context end
  cmd = cmd .. " -- " .. shell_escape(pattern) .. " " .. shell_escape(path)
  
  local success, res = call_tool(tools.execute_bash, {command = cmd})
  if not success then
    if res:find("status 1") then
      res = ""
    else
      error(res)
    end
  end
  
  local lines = {}
  local count = 0
  for line in res:gmatch("[^\r\n]+") do
    count = count + 1
    if count <= 50 then
      table.insert(lines, line)
    end
  end
  
  local output = table.concat(lines, "\n")
  if count > 50 then
    output = output .. "\n[TRUNCATED: Use a more specific pattern or path to narrow results]"
  end
  
  return output
end

function tools.search_code(args)
  return tools.grep_tool({pattern = args.query, path = "."})
end

-- Mail Model Support (Git Tools)

function tools.git_branch_staging(args)
  local name = args.name
  local base_branch = args.base_branch or "main"
  local staging_name = "slop/staging/" .. name
  
  local cmd = string.format("git checkout -b %s %s", shell_escape(staging_name), shell_escape(base_branch))
  local success, res = call_tool(tools.execute_bash, {command = cmd})
  if not success then error("Failed to create staging branch: " .. tostring(res)) end
  
  return "Created and checked out staging branch: " .. staging_name .. " (base: " .. base_branch .. ")"
end

function tools.git_commit_patch(args)
  slop_guard()
  local summary = args.summary
  local rationale = args.rationale
  
  local full_message = summary .. "\n\nRationale: " .. rationale
  local cmd = string.format("git commit -m %s", shell_escape(full_message))
  local success, res = call_tool(tools.execute_bash, {command = cmd})
  if not success then error("Commit failed: " .. tostring(res)) end
  
  return tools.git_format_patch_series({})
end

function tools.git_reroll_patch(args)
  slop_guard()
  local index = args.index
  local base_branch = args.base_branch or "main"
  
  -- Find the commit hash for the patch at index
  local log_cmd = string.format("git log %s..HEAD --oneline --reverse", shell_escape(base_branch))
  local success, log_res = call_tool(tools.execute_bash, {command = log_cmd})
  if not success then error("Failed to get log: " .. tostring(log_res)) end
  
  local commits = {}
  for hash in log_res:gmatch("(%w+) ") do table.insert(commits, hash) end
  
  if index < 1 or index > #commits then
    error(string.format("Invalid patch index %d. Series has %d patches.", index, #commits))
  end
  
  local target_hash = commits[index]
  
  -- Fixup: stage changes, commit as fixup, then rebase
  local fixup_cmd = string.format("git add -A && git commit --fixup=%s && GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash %s~1", target_hash, target_hash)
  local success2, rebase_res = call_tool(tools.execute_bash, {command = fixup_cmd})
  if not success2 then
    error("Reroll failed: " .. tostring(rebase_res))
  end
  
  return tools.git_format_patch_series({})
end

function tools.git_verify_series(args)
  local command = args.command
  local base_branch = args.base_branch or "main"
  
  local log_cmd = string.format("git log %s..HEAD --oneline --reverse", shell_escape(base_branch))
  local success, log_res = call_tool(tools.execute_bash, {command = log_cmd})
  if not success then error("Failed to get log: " .. tostring(log_res)) end
  
  local commits = {}
  for hash in log_res:gmatch("(%w+) ") do table.insert(commits, hash) end
  
  local current_branch = git.get_current_branch()
  local results = {}
  
  for i, hash in ipairs(commits) do
    call_tool(tools.execute_bash, {command = "git checkout " .. hash})
    local success_test, test_res = call_tool(tools.execute_bash, {command = command})
    table.insert(results, string.format("Patch [%d/%d] (%s): %s", i, #commits, hash, success_test and "PASSED" or "FAILED"))
    if not success_test then
      call_tool(tools.execute_bash, {command = "git checkout " .. current_branch})
      return table.concat(results, "\n") .. "\n\nVerification failed at patch " .. i .. ":\n" .. test_res
    end
  end
  
  call_tool(tools.execute_bash, {command = "git checkout " .. current_branch})
  return table.concat(results, "\n") .. "\n\nAll patches verified successfully."
end

function tools.git_format_patch_series(args)
  local base_branch = args.base_branch or "main"
  
  -- Use a custom format to extract subject and rationale
  local log_format = "---COMMIT_START---%n%H%n%an%n%ae%n%ad%n%s%n%b"
  local log_cmd = string.format("git log %s..HEAD --format=%s", shell_escape(base_branch), shell_escape(log_format))
  
  local success, log_res = call_tool(tools.execute_bash, {command = log_cmd})
  if not success then error("Failed to get series log: " .. tostring(log_res)) end
  
  local diff_cmd = string.format("git diff %s..HEAD", shell_escape(base_branch))
  local diff_success, diff_res = pcall(tools.execute_bash, {command = diff_cmd})
  if not diff_success then
    error("Failed to get diff: " .. diff_res)
  end

  -- Parse commits and format them
  local patches = {}
  for commit_data in log_res:gmatch("---COMMIT_START---%s*(.-)%s*---COMMIT_START---") do
    table.insert(patches, commit_data)
  end
  -- Catch the last one
  local last_commit = log_res:match("---COMMIT_START---%s*([^-]+)$")
  if last_commit then table.insert(patches, last_commit) end

  local formatted_patches = {}
  for i, patch in ipairs(patches) do
    local lines = {}
    for line in patch:gmatch("([^\n]*)\n?") do table.insert(lines, line) end
    
    local hash = lines[1]
    local author = lines[2]
    local email = lines[3]
    local date = lines[4]
    local subject = lines[5]
    local body = ""
    for j=6,#lines do body = body .. lines[j] .. "\n" end

    -- Extract rationale if present in body
    local rationale = body:match("Rationale: (.-)\n") or "No rationale provided."
    
    local formatted = string.format("### Patch [%d/%d]: %s\nRationale: %s\n ###\ncommit %s\nAuthor: %s <%s>\nDate:   %s\n\n    %s\n\n%s",
      i, #patches, subject, rationale, hash, author, email, date, subject, body)
    table.insert(formatted_patches, formatted)
  end

  local output = table.concat(formatted_patches, "\n")
  output = output .. "\n\n" .. diff_res
  return output
end

function tools.git_finalize_series(args)
  slop_guard()

  local current_branch = git.get_current_branch()
  local target_branch = args.target_branch or "main"
  
  -- 1. Verify approval
  local success, hash = call_tool(tools.execute_bash, {command = "git rev-parse HEAD"})
  if not success then error("Failed to get current hash: " .. tostring(hash)) end
  hash = hash:gsub("%s+", "")

  local approval_query = string.format("SELECT approved_hash FROM patch_approvals WHERE branch_name = %s", shell_escape(current_branch))
  local success2, approval_res = call_tool(tools.query_db, {sql = approval_query})
  if not success2 then error("Failed to query approvals: " .. tostring(approval_res)) end
  
  if not approval_res:find(hash) then
    error("Patch series not approved or hash mismatch. Please obtain approval for hash " .. hash .. " before finalizing.")
  end

  -- 2. Merge into target
  local checkout_cmd = "git checkout " .. shell_escape(target_branch)
  local success3, checkout_res = call_tool(tools.execute_bash, {command = checkout_cmd})
  if not success3 then
    error("Failed to checkout target branch '" .. target_branch .. "': " .. tostring(checkout_res))
  end

  local merge_cmd = "git merge --ff-only " .. shell_escape(current_branch)
  local success4, merge_res = call_tool(tools.execute_bash, {command = merge_cmd})
  if not success4 then
    error("Merge failed: " .. tostring(merge_res))
  end

  -- 3. Cleanup
  pcall(tools.execute_bash, {command = "git branch -D " .. shell_escape(current_branch)})

  return "Successfully finalized series. Merged " .. current_branch .. " into " .. target_branch .. " and deleted staging branch."
end

function llm_query(query)
  if not query or query == "" then error("llm_query requires a query string") end
  local escaped = query:gsub("'", "'\\''")
  local success, result = call_tool(tools.execute_bash, {command = "std_slop --prompt '" .. escaped .. "'"})
  if not success then error("llm_query failed: " .. result) end
  return result
end

-- Also available in the tools table for consistency with C++ tools
tools.llm_query = function(args)
  return llm_query(args.query)
end

function get_tool_manifest()
  local m = { tools = {} }
  for name, _ in pairs(tools) do
    table.insert(m.tools, name)
  end
  return m
end
