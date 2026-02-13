-- Slop Lua Preamble

-- Helper to escape shell arguments
function shell_escape(s)
  if type(s) ~= "string" then s = tostring(s) end
  return "'" .. string.gsub(s, "'", "'\\''") .. "'"
end

-- Git Helpers
git = git or {}

function git.get_current_branch()
  local forced = os.getenv("SLOP_FORCE_BRANCH_NAME")
  if forced and forced ~= "" then return forced end
  
  local success, branch = tools.execute_bash({command = "git rev-parse --abbrev-ref HEAD 2>/dev/null"})
  if not success then return nil end
  return branch:gsub("%s+", "")
end

function git.get_base_branch(requested_base)
  if requested_base and requested_base ~= "" then
    return requested_base
  end

  -- 1. Try git config
  local success, base = tools.execute_bash({command = "git config slop.basebranch"})
  if success then
    base = base:gsub("%s+", "")
    if base ~= "" then return base end
  end

  -- 2. Try upstream
  success, base = tools.execute_bash({command = "git rev-parse --abbrev-ref @{u} 2>/dev/null"})
  if success then
    base = base:gsub("%s+", "")
    if base ~= "" then return base end
  end

  error("Could not determine the upstream (base) branch. Please set the base branch using 'git config slop.basebranch <branch>'.")
end

function git.get_patch_series_summary(base)
  local cmd = "git log --oneline --reverse " .. shell_escape(base) .. "..HEAD"
  local success, log = tools.execute_bash({command = cmd})
  
  if not success or log:gsub("%s+", "") == "" then
    return "\n\nNo patches in series (base: " .. base .. ")"
  end

  return "\n\n--- Current Patch Series (base: " .. base .. ") ---\n" .. log
end

-- Guard for protected tools
function slop_guard()
  if os.getenv("SLOP_SKIP_STAGING_CHECK") == "1" then
    return
  end
  
  local branch = git.get_current_branch()
  if not branch or branch == "" then return end
  
  if not branch:find("^slop/staging/") then
    error("Mail Model Violation: Branch '" .. branch .. "' is not a staging branch. " ..
          "Please use 'git_branch_staging' to create a new staging branch.")
  end
end

function llm_query(query)
  if not query or query == "" then error("llm_query requires a query string") end
  local escaped = query:gsub("'", "'\\''")
  local success, result = tools.execute_bash({command = "std_slop --prompt '" .. escaped .. "'"})
  if not success then error("llm_query failed: " .. result) end
  return result
end

-- Tool Implementations in Lua
tools = tools or {}

function tools.grep_tool(args)
  local pattern = args.pattern
  local path = args.path or "."
  
  local success, is_git = tools.execute_bash({command = "git rev-parse --is-inside-work-tree 2>/dev/null"})
  local is_git_repo = success and is_git:gsub("%s+", "") == "true"
  
  local ok, res
  if is_git_repo then
    ok, res = tools.git_grep_tool({pattern = pattern, path = path})
  else
    local cmd = "grep -rnE -- " .. shell_escape(pattern) .. " " .. shell_escape(path)
    ok, res = tools.execute_bash({command = cmd})
  end
  
  if not ok then
    return ""
  end
  
  return res
end

function tools.search_code(args)
  return tools.grep_tool({pattern = args.query, path = "."})
end

-- Also available in the tools table for consistency with C++ tools
tools.llm_query = function(args)
  return llm_query(args.query)
end

manifest = { tools = {} }
for name, _ in pairs(tools) do
  table.insert(manifest.tools, name)
end
