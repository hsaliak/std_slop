function tools.read_file(args)
  if type(args.path) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: path", 0) end
  if args.start_line ~= nil and type(args.start_line) ~= "number" then error("INVALID_ARGUMENT: 'start_line' must be an integer", 0) end
  if args.end_line ~= nil and type(args.end_line) ~= "number" then error("INVALID_ARGUMENT: 'end_line' must be an integer", 0) end

  local path = args.path

  -- SECURITY: Prevent basic path traversal and absolute path escapes
  if string.find(path, "%.%.") or string.sub(path, 1, 1) == "/" then
    error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.", 0)
  end

  local f, err = io.open(path, "r")
  if not f then error("Could not open file: " .. (err or path)) end

  local start_line = args.start_line or 1
  local end_line = args.end_line or math.huge -- Use math.huge to represent EOF if no end is specified

  if start_line > end_line then
    f:close()
    error("INVALID_ARGUMENT: start_line must be less than or equal to end_line", 0)
  end
  if start_line < 1 then start_line = 1 end

  local body_lines = {}
  local current_line = 1

  -- OPTIMIZATION: Stream the file and stop early
  for line in f:lines() do
    if current_line > end_line then
      break -- We hit the end_line limit, stop reading disk entirely!
    end

    if current_line >= start_line then
      if args.line_numbers then
        line = string.format("%d: %s", current_line, line)
      end
      table.insert(body_lines, line)
    end

    current_line = current_line + 1
  end
  f:close()

  if start_line >= current_line then
    return "" -- The start_line was beyond the EOF
  end

  local body = table.concat(body_lines, "\n")
  if #body > 0 then body = body .. "\n" end

  return body
end

function tools.write_file(args)
  slop_guard() -- Require staging branch for writing
  
  -- 1. Validate inputs before assignment
  if type(args.path) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: path", 0) end
  if type(args.content) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: content", 0) end

  local path = args.path
  local content = args.content

  -- 2. SECURITY: Prevent path traversal and absolute paths
  if string.find(path, "%.%.") or string.sub(path, 1, 1) == "/" then
    error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.", 0)
  end

  -- 3. Attempt to open the file
  local f, err = io.open(path, "w")
  
  -- 4. Handle the "Missing Directory" edge case specifically for the LLM
  if not f then 
    -- If it failed, it might be because the parent directory doesn't exist.
    -- We give the LLM a highly specific error so it knows to create the dir first.
    error(string.format(
      "Could not open file for writing: %s\n" ..
      "Hint: Does the directory exist? Lua cannot create nested directories automatically. " ..
      "Use tools.execute_bash({command='mkdir -p <dir>'}) first if needed. Original error: %s", 
      path, err
    ), 0)
  end

  local ok, write_err = f:write(content)
  if not ok then
    f:close()
    error("IO_ERROR: Failed to write to file: " .. tostring(write_err), 0)
  end
  
  local close_ok, close_err = f:close()
  if not close_ok then
    error("IO_ERROR: Failed to close file: " .. tostring(close_err), 0)
  end

  local result = "File written successfully:\n"
  result = result .. "Path: " .. path .. "\n"
  result = result .. "Bytes written: " .. #content .. "\n"
  return result
end

function tools.execute_bash(args)
  slop_guard()
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

function tools.execute_bash_async(args)
  slop_guard()
  if not args.command then
    error("Usage: execute_bash_async({command = '...'})", 0)
  end
  return tools.dispatch_async("execute_bash", args)
end

-- Knowledge Management Tools

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

function tools.grep(args)
  local patterns = args.patterns or {}
  if args.pattern then table.insert(patterns, args.pattern) end
  
  if #patterns == 0 then
    error("grep requires at least one pattern (use 'pattern').")
  end

  local path = args.path or "."
  local paths = type(path) == "table" and path or {path}

  local res = ""
  -- Try git grep first
  local git_check_ok, git_check_res = call_tool(tools.execute_bash, {command = "git rev-parse --is-inside-work-tree"})
  local is_git = git_check_ok and git_check_res:find("true")
  
  if is_git then
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
    for _, p in ipairs(patterns) do cmd = cmd .. " -e " .. shell_escape(p) end
    cmd = cmd .. " --"
    for _, p in ipairs(paths) do cmd = cmd .. " " .. shell_escape(p) end
    
    local success, git_res = call_tool(tools.execute_bash, {command = cmd})
    if success and git_res and git_res ~= "" then
      res = git_res
    elseif not success and not git_res:find("status 1") then
      error(git_res)
    end
  end
  
  -- Fallback to standard grep if git grep returned nothing
  if res == "" then
    local cmd = "grep -rnE"
    if args.context and args.context > 0 then cmd = cmd .. " -C " .. args.context end
    if args.case_insensitive then cmd = cmd .. " -i" end
    
    for _, p in ipairs(patterns) do
      cmd = cmd .. " -e " .. shell_escape(p)
    end
    
    for _, p in ipairs(paths) do
      cmd = cmd .. " " .. shell_escape(p)
    end
    
    local success, grep_res = call_tool(tools.execute_bash, {command = cmd})
    if success then
      res = grep_res
    elseif not grep_res:find("status 1") then
      error(grep_res)
    end
  end
  
  -- Apply truncation
  local limit = tonumber(args.limit) or 500
  local lines = {}
  local count = 0
  for line in res:gmatch("[^\r\n]+") do
    count = count + 1
    if count <= limit then
      table.insert(lines, line)
    end
  end
  
  local output = table.concat(lines, "\n")
  if count > limit then
    output = output .. "\n[TRUNCATED: Use a more specific pattern or path to narrow results]"
  end
  
  return output
end

function tools.grep_tool(args) return tools.grep(args) end


-- Mail Model Support (Git Tools)

function tools.apply_patch(args)
  slop_guard()
  if type(args.path) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: path", 0) end
  if type(args.patches) ~= "table" then error("INVALID_ARGUMENT: Missing mandatory field: patches", 0) end

  local path = args.path

  -- SECURITY: Prevent path traversal
  if string.find(path, "%.%.") or string.sub(path, 1, 1) == "/" then
    error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.", 0)
  end

  local f = io.open(path, "r")
  if not f then
    error("NOT_FOUND: Could not open file: " .. path, 0)
  end
  local content = f:read("*all")
  f:close()

  -- AI UX FIX: Normalize file content newlines to standard \n
  content = string.gsub(content, "\r\n", "\n")

  for i, patch in ipairs(args.patches) do
    local find = patch.find
    local replace = patch.replace
    if type(find) ~= "string" or type(replace) ~= "string" then
      error("INVALID_ARGUMENT: each patch must have 'find' and 'replace' strings", 0)
    end

    -- AI UX FIX: Normalize LLM patch newlines as well
    find = string.gsub(find, "\r\n", "\n")
    replace = string.gsub(replace, "\r\n", "\n")

    local start_idx, end_idx = string.find(content, find, 1, true)
    if not start_idx then
      -- Hint added so the LLM knows *why* it failed and how to fix it
      error("NOT_FOUND: Could not find exact match for the 'find' block in: " .. path ..
            "\nHint: Ensure your indentation matches the target file exactly, and provide more context lines if needed.", 0)
    end

    local second_start = string.find(content, find, end_idx + 1, true)
    if second_start then
      error("FAILED_PRECONDITION: Multiple matches found for the 'find' block. " ..
            "Please use a more specific 'find' block with more surrounding context lines.", 0)
    end

    content = string.sub(content, 1, start_idx - 1) .. replace .. string.sub(content, end_idx + 1)
  end

  local f_out = io.open(path, "w")
  if not f_out then
    error("Could not open file for writing: " .. path, 0)
  end
  f_out:write(content)
  f_out:close()

  return "File patched successfully: " .. path
end
