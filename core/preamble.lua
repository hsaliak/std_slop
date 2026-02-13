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

function tools.git_branch_staging(args)
  local name = args.name
  if not name or name == "" then
    error("git_branch_staging requires a 'name' for the staging branch.")
  end

  local base = git.get_base_branch(args.base_branch)
  local branch_name = "slop/staging/" .. name

  -- Check if branch already exists
  local check_cmd = "git rev-parse --verify " .. shell_escape(branch_name) .. " 2>/dev/null"
  local exists, _ = tools.execute_bash({command = check_cmd})
  if exists then
    error("Branch '" .. branch_name .. "' already exists. Please choose a different name.")
  end

  -- Create and checkout the branch
  local create_cmd = "git checkout -b " .. shell_escape(branch_name) .. " " .. shell_escape(base)
  local success, res = tools.execute_bash({command = create_cmd})
  if not success then
    error("Failed to create staging branch: " .. res)
  end

  -- Store base branch in git config
  tools.execute_bash({command = "git config slop.basebranch " .. shell_escape(base)})

  return "Created and checked out staging branch: " .. branch_name .. " (base: " .. base .. ")"
end

function tools.git_commit_patch(args)
  slop_guard()

  local summary = args.summary
  local rationale = args.rationale

  if not summary or summary == "" then
    error("git_commit_patch requires a 'summary'.")
  end
  if not rationale or rationale == "" then
    error("git_commit_patch requires a 'rationale'.")
  end

  -- Stage all changes
  tools.execute_bash({command = "git add -A"})

  -- Commit
  local cmd = "git commit -m " .. shell_escape(summary) .. " -m " .. shell_escape("Rationale: " .. rationale)
  local success, res = tools.execute_bash({command = cmd})
  if not success then
    error("Failed to commit patch: " .. res)
  end

  local base = git.get_base_branch()
  return "Committed patch: " .. summary .. git.get_patch_series_summary(base)
end

function tools.git_reroll_patch(args)
  slop_guard()

  local index = tonumber(args.index)
  if not index or index <= 0 then
    error("git_reroll_patch requires a 1-based 'index'.")
  end

  local base = git.get_base_branch(args.base_branch)

  -- 1. Get list of commits
  local log_cmd = "git rev-list --reverse " .. shell_escape(base) .. "..HEAD"
  local success, log_res = tools.execute_bash({command = log_cmd})
  if not success then
    error("Failed to get commit list: " .. log_res)
  end

  local commits = {}
  for hash in log_res:gmatch("%S+") do
    table.insert(commits, hash)
  end

  if index > #commits then
    error("Patch index " .. index .. " exceeds series length (" .. #commits .. ").")
  end

  local target_hash = commits[index]

  -- 2. Stage changes
  tools.execute_bash({command = "git add ."})

  -- Check if there are changes
  local diff_success, _ = tools.execute_bash({command = "git diff --cached --quiet"})
  if diff_success then
    return "No changes found to reroll into patch " .. index
  end

  -- 3. Create fixup commit
  local fixup_cmd = "git commit --fixup " .. shell_escape(target_hash)
  local fixup_success, fixup_res = tools.execute_bash({command = fixup_cmd})
  if not fixup_success then
    error("Failed to create fixup commit: " .. fixup_res)
  end

  -- 4. Autosquash rebase
  local rebase_cmd = "GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash " .. shell_escape(base)
  local rebase_success, rebase_res = tools.execute_bash({command = rebase_cmd})
  if not rebase_success then
    error("Autosquash rebase failed: " .. rebase_res)
  end

  return "Successfully rerolled changes into patch " .. index .. git.get_patch_series_summary(base)
end

function tools.git_verify_series(args)
  slop_guard()

  local command = args.command
  if not command or command == "" then
    error("git_verify_series requires a 'command' to run for each patch.")
  end

  local base = git.get_base_branch(args.base_branch)
  local original_branch = git.get_current_branch()

  -- 1. Get list of commits
  local log_cmd = "git rev-list --reverse " .. shell_escape(base) .. "..HEAD"
  local success, log_res = tools.execute_bash({command = log_cmd})
  if not success then
    error("Failed to get commit list: " .. log_res)
  end

  local commits = {}
  for hash in log_res:gmatch("%S+") do
    table.insert(commits, hash)
  end

  local report = {}
  local all_passed = true

  for i, hash in ipairs(commits) do
    -- Checkout commit
    local checkout_success, checkout_res = tools.execute_bash({command = "git checkout " .. shell_escape(hash)})
    if not checkout_success then
      all_passed = false
      table.insert(report, {
        patch_index = i,
        hash = hash,
        status = "failed",
        error = "Checkout failed: " .. checkout_res
      })
    else
      -- Run verification command
      local verify_success, verify_res = tools.execute_bash({command = command})
      local item = {
        patch_index = i,
        hash = hash,
        status = verify_success and "passed" or "failed"
      }
      if not verify_success then
        all_passed = false
        item.stderr = verify_res
      end
      table.insert(report, item)
    end
  end

  -- Return to original branch
  tools.execute_bash({command = "git checkout " .. shell_escape(original_branch)})

  -- We return a JSON string to match the expected tool output format
  -- In Lua, we can use a helper or just build it.
  -- Since the tool result is eventually converted back to string, we can just return a formatted string.
  
  local res_str = "Verification Results:\n"
  for _, item in ipairs(report) do
    res_str = res_str .. string.format("[%d] %s: %s\n", item.patch_index, item.hash:sub(1,7), item.status)
    if item.status == "failed" then
      res_str = res_str .. "    Error: " .. (item.error or item.stderr or "unknown") .. "\n"
    end
  end
  
  if all_passed then
    res_str = res_str .. "\nALL PATCHES PASSED."
  else
    res_str = res_str .. "\nSOME PATCHES FAILED."
  end

  return res_str
end

function tools.git_format_patch_series(args)
  local base = git.get_base_branch(args.base_branch)

  local format = "### Patch [%N/%T]: %s%nRationale: %b%n ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b"
  -- Note: We need to handle the %N and %T separately as git doesn't support them directly in --format
  -- but we can use a simpler format and post-process.
  
  local log_cmd = "git log --reverse --format='---COMMIT_START---%n%H%n%an%n%ae%n%ad%n%s%n%b' " .. shell_escape(base) .. "..HEAD"
  local log_success, log_res = tools.execute_bash({command = log_cmd})
  if not log_success then
    error("Failed to get commit log: " .. log_res)
  end

  local diff_cmd = "git diff " .. shell_escape(base) .. "..HEAD"
  local diff_success, diff_res = tools.execute_bash({command = diff_cmd})
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
    -- Remove Rationale line from body for the final output if we want to mimic C++
    
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
  local success, hash = tools.execute_bash({command = "git rev-parse HEAD"})
  if not success then error("Failed to get current hash: " .. hash) end
  hash = hash:gsub("%s+", "")

  local approval_query = string.format("SELECT approved_hash FROM patch_approvals WHERE branch_name = %s", shell_escape(current_branch))
  local approval_res = tools.query_db({sql = approval_query})
  
  -- query_db returns a JSON string (list of objects)
  -- We need to parse it or check if it contains the hash.
  -- In this environment, we can check if the result string contains the hash.
  if not approval_res:find(hash) then
    error("Patch series not approved or hash mismatch. Please obtain approval for hash " .. hash .. " before finalizing.")
  end

  -- 2. Merge into target
  local checkout_cmd = "git checkout " .. shell_escape(target_branch)
  local checkout_success, checkout_res = tools.execute_bash({command = checkout_cmd})
  if not checkout_success then
    error("Failed to checkout target branch '" .. target_branch .. "': " .. checkout_res)
  end

  local merge_cmd = "git merge --ff-only " .. shell_escape(current_branch)
  local merge_success, merge_res = tools.execute_bash({command = merge_cmd})
  if not merge_success then
    -- Attempt a regular merge if ff-only fails? C++ used --ff-only for safety usually.
    -- Let's stick to ff-only or simple merge.
    error("Merge failed: " .. merge_res)
  end

  -- 3. Cleanup
  tools.execute_bash({command = "git branch -D " .. shell_escape(current_branch)})

  return "Successfully finalized series. Merged " .. current_branch .. " into " .. target_branch .. " and deleted staging branch."
end

-- Also available in the tools table for consistency with C++ tools
tools.llm_query = function(args)
  return llm_query(args.query)
end

manifest = { tools = {} }
for name, _ in pairs(tools) do
  table.insert(manifest.tools, name)
end
