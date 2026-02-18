function tools.git_branch_staging(args)
  local name = args.name
  local base_branch = args.base_branch or git.get_current_branch()
  local staging_name = "slop/staging/" .. name
  
  local cmd = string.format("git checkout -b %s %s", shell_escape(staging_name), shell_escape(base_branch))
  local res = __os_run(cmd)
  if res.exit_code ~= 0 then error("Failed to create staging branch: " .. res.stdout .. res.stderr) end

  -- Record the parent branch for stickiness
  tools.query_db({
    sql = "INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
    params = {staging_name, base_branch}
  })
  
  return "Created and checked out staging branch: " .. staging_name .. " (base: " .. base_branch .. ")"
end

function tools.git_commit_patch(args)
  slop_guard()
  local summary = args.summary
  local rationale = args.rationale
  
  if #summary > 50 then error("Summary must be <= 50 characters") end
  
  local full_msg = summary .. "\n\n" .. rationale
  local cmd = string.format("git commit -m %s", shell_escape(full_msg))
  local res = __os_run(cmd)
  if res.exit_code ~= 0 then error("Commit failed: " .. res.stdout .. res.stderr) end
  
  return tools.git_format_patch_series({})
end

function tools.git_reroll_patch(args)
  slop_guard()
  local index = tonumber(args.index)
  local base_branch = git.resolve_base_branch(args.base_branch)
  
  -- 1. Get the list of commits
  local log_cmd = string.format("git log --reverse --format=%%H %s..HEAD", shell_escape(base_branch))
  local log_success, log_res = pcall(tools.execute_bash, {command = log_cmd})
  if not log_success then error("Failed to get commit list: " .. tostring(log_res)) end
  
  local commits = {}
  for hash in log_res:gmatch("%S+") do table.insert(commits, hash) end
  
  if index < 1 or index > #commits then
    error(string.format("Invalid patch index %d (total patches: %d)", index, #commits))
  end
  
  -- 2. Perform the rebase
  local target_hash = commits[index]
  
  local commit_res = __os_run("git commit --fixup " .. target_hash)
  if commit_res.exit_code ~= 0 then error("Failed to create fixup commit. Are there any changes staged?") end
  
  -- Use non-interactive rebase
  local env_cmd = "GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash " .. shell_escape(base_branch)
  local rebase_res = __os_run(env_cmd)
  if rebase_res.exit_code ~= 0 then
    __os_run("git rebase --abort")
    error("Rebase failed. You may have conflicts. Manual intervention required.\n" .. rebase_res.stderr)
  end
  
  return tools.git_format_patch_series({base_branch = base_branch})
end

function tools.git_verify_series(args)
  slop_guard()
  local command = args.command
  local base_branch = git.resolve_base_branch(args.base_branch)
  
  local log_cmd = string.format("git log --reverse --format=%%H %s..HEAD", shell_escape(base_branch))
  local log_success, log_res = pcall(tools.execute_bash, {command = log_cmd})
  if not log_success then error("Failed to get commit list: " .. tostring(log_res)) end
  
  local commits = {}
  for hash in log_res:gmatch("%S+") do table.insert(commits, hash) end
  
  local current_branch = git.get_current_branch()
  local results = {}
  local all_passed = true
  
  for i, hash in ipairs(commits) do
    __os_run("git checkout " .. hash)
    local test_res = __os_run(command)
    local status = (test_res.exit_code == 0) and "PASSED" or "FAILED"
    table.insert(results, string.format("Patch [%d/%d] (%s...): %s", i, #commits, hash:sub(1,7), status))
    if not (test_res.exit_code == 0) then 
      all_passed = false 
      break
    end
  end
  
  -- Restore state
  __os_run("git checkout " .. shell_escape(current_branch))
  
  local report = table.concat(results, "\n")
  if all_passed then
    return "Verification Successful!\n\n" .. report
  else
    return "Verification FAILED!\n\n" .. report
  end
end

function tools.git_format_patch_series(args)
  slop_guard()
  local base_branch = git.resolve_base_branch(args.base_branch)
  
  local log_cmd = string.format("git log --reverse --format='### Patch [%%n/%%N] ###%%ncommit %%H%%nAuthor: %%an <%%ae>%%nDate:   %%ad%%n%%n    %%s%%n%%n%%b' %s..HEAD", shell_escape(base_branch))
  local log_success, log_res = pcall(tools.execute_bash, {command = log_cmd})
  if not log_success then error("Failed to get patch logs: " .. tostring(log_res)) end
  
  local diff_cmd = string.format("git diff %s..HEAD", shell_escape(base_branch))
  local diff_success, diff_res = pcall(tools.execute_bash, {command = diff_cmd})
  if not diff_success then error("Failed to get diff: " .. tostring(diff_res)) end
  
  return "--- MAIL SERIES ---\nBase: " .. base_branch .. "\n\n" .. log_res .. "\n\n--- FULL DIFF ---\n" .. diff_res
end

function tools.git_finalize_series(args)
  slop_guard()

  local current_branch = git.get_current_branch()
  local target_branch = git.resolve_base_branch(args.target_branch)

  -- 1. Verify approval
  local success, hash = call_tool(tools.execute_bash, {command = "git rev-parse HEAD"})
  if not success then error("Failed to get current hash: " .. tostring(hash)) end
  hash = hash:gsub("%s+", "")

  local approval_res = tools.query_db({
    sql = "SELECT approved_hash FROM patch_approvals WHERE branch_name = ?",
    params = {current_branch}
  })
  
  if not approval_res:find(hash) then
    error("Patch series not approved or hash mismatch. Please obtain approval for hash " .. hash .. " before finalizing.")
  end

  -- 2. Merge into target
  local checkout_cmd = "git checkout " .. shell_escape(target_branch)
  local res1 = __os_run(checkout_cmd)
  if res1.exit_code ~= 0 then
    error("Failed to checkout target branch '" .. target_branch .. "': " .. res1.stderr)
  end

  local merge_cmd = "git merge --ff-only " .. shell_escape(current_branch)
  local res2 = __os_run(merge_cmd)
  if res2.exit_code ~= 0 then
    -- Try to switch back before erroring
    __os_run("git checkout " .. shell_escape(current_branch))
    error("Merge failed: " .. res2.stderr)
  end

  -- 3. Cleanup
  __os_run("git branch -D " .. shell_escape(current_branch))
  
  -- Remove metadata from database
  tools.query_db({
    sql = "DELETE FROM staging_branches WHERE branch_name = ?",
    params = {current_branch}
  })
  tools.query_db({
    sql = "DELETE FROM patch_approvals WHERE branch_name = ?",
    params = {current_branch}
  })

  return "Successfully finalized series and merged into " .. target_branch
end

