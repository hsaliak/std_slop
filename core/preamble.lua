-- Slop Lua Preamble

-- Helper to escape shell arguments
function shell_escape(s)
  if type(s) ~= "string" then s = tostring(s) end
  return "'" .. string.gsub(s, "'", "'\\''") .. "'"
end

-- Guard for protected tools
function slop_guard()
  if os.getenv("SLOP_SKIP_STAGING_CHECK") == "1" then
    return
  end
  
  -- Check if we are in a git repo and get current branch
  local success, branch = tools.execute_bash({command = "git rev-parse --abbrev-ref HEAD 2>/dev/null"})
  if not success then 
    -- If not a git repo, we can't be on a staging branch.
    return 
  end
  
  branch = branch:gsub("%s+", "")
  if branch == "" then return end
  
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
