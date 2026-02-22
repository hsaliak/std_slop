--[[
Slop Preamble Library
--------------------
This library defines the Lua environment for the 'run_lua' tool.
It wraps native C++ tools to provide a more idiomatic Lua interface
and implements the 'Mail Model' workflow helpers.

Key Globals:
- tools: Table containing all tool functions (e.g., tools.read_file).
- state, scratchpad, history: Injected session context.

For a full API reference, call 'tools.help()'.
]]

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

-- Capture native tools to avoid recursion when wrapping
local native = {}
for k, v in pairs(tools) do
    native[k] = v
end

-- Internal state tracking
local _loaded_session = nil
local _initial_scratchpad_json = "{}"
local _initial_state = nil

function core.load_session_state()
  -- Skip if already loaded (e.g. via C++ injection)
  if history and #history > 0 then
    return
  end
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
    local raw_sp = rows[1].scratchpad or "{}"
    local ok, parsed = pcall(JSON.parse, raw_sp)
    
    -- Guarantee scratchpad is a table for the duration of the session
    scratchpad = (ok and type(parsed) == "table") and parsed or {}
    -- Anchor the initial state for change detection
    _initial_scratchpad_json = JSON.stringify(scratchpad)
    window_size = rows[1].context_size or 0
  else
    scratchpad = {}
    _initial_scratchpad_json = "{}"
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
  
  -- Use serialization for deep-comparison to detect any changes in the table
  local current_json = JSON.stringify(scratchpad)
  if current_json ~= _initial_scratchpad_json then
    tools.query_db({
      sql = "INSERT INTO sessions (id, scratchpad) VALUES (?, ?) " ..
            "ON CONFLICT(id) DO UPDATE SET scratchpad = excluded.scratchpad",
      params = {session_id, current_json}
    })
    _initial_scratchpad_json = current_json
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
  if name == "manage_scratchpad" then
    if result == nil or result == "" or (type(result) == "table" and next(result) == nil) then
      return string.format("### TOOL_RESULT: %s\nScratchpad is empty\n---", name)
    end
  end
  if type(result) == "table" then
    local ok, res_str = pcall(JSON.stringify, result)
    if ok then
      return string.format("### TOOL_RESULT: %s\n%s\n---", name, res_str)
    end
    local items = {}
    for k, v in pairs(result) do
      table.insert(items, tostring(k) .. ": " .. tostring(v))
    end
    return string.format("### TOOL_RESULT: %s\n%s\n---", name, table.concat(items, "\n"))
  end
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
  -- Check database for mode. If 'standard', we bypass the guard.
  local ok, res = pcall(tools.query_db, {sql = "SELECT mode FROM settings WHERE id = 1"})
  if ok and res and string.find(res, '"standard"') then
    return
  end

  local branch = git.get_current_branch()
  if not branch then return end -- Allow if not in a git repo (e.g. during unit tests)

  if not branch:find("^slop/staging/") and branch ~= "HEAD" then
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
    return core.wrap_result(name, "Error: " .. tostring(result))
  end
  return core.wrap_result(name, result)
end

-- Git Helpers
git = git or {}

function git.get_current_branch()
  local forced = os.getenv("SLOP_FORCE_BRANCH_NAME")
  if forced and forced ~= "" then return forced end
  
  local res = __os_run("git rev-parse --abbrev-ref HEAD 2>/dev/null")
  if res.exit_code ~= 0 then return nil end
  return res.stdout:gsub("%s+", "")
end

function git.is_staging_branch()
  local branch = git.get_current_branch()
  return branch and branch:find("^slop/staging/") ~= nil
end

function git.get_base_branch(requested_base)
  if requested_base and requested_base ~= "" then return requested_base end
  
  local current = git.get_current_branch()
  if not current then return "main" end
  
  -- The ONLY source of truth: the staging_branches table
  local res_json = tools.query_db({
    sql = "SELECT parent_branch FROM staging_branches WHERE branch_name = ?",
    params = {current}
  })
  
  if res_json then
    local parent = res_json:match('"parent_branch":"(.-)"')
    if parent then return parent end
  end
  
  if current:find("^slop/staging/") then
    error("Base branch not found in database for staging branch '" .. current .. "'.")
  end
  
  return "main"
end

function git.resolve_base_branch(requested)
  return git.get_base_branch(requested)
end


-- Foundation Tools (Migrated from C++)

function get_tool_manifest()
  local m = { tools = {} }
  for name, _ in pairs(tools) do
    table.insert(m.tools, name)
  end
  return m
end
