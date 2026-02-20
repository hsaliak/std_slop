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
  local path = args.path
  local content = args.content
  if type(args.path) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: path", 0) end
  if type(args.content) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: content", 0) end

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

function tools.apply_patch(args)
  slop_guard()
  if type(args.path) ~= "string" then error("INVALID_ARGUMENT: Missing mandatory field: path", 0) end
  if type(args.patches) ~= "table" then error("INVALID_ARGUMENT: Missing mandatory field: patches", 0) end

  local f = io.open(args.path, "r")
  if not f then
    error("NOT_FOUND: Could not open file: " .. args.path, 0)
  end
  local content = f:read("*all")
  f:close()

  for i, patch in ipairs(args.patches) do
    local find = patch.find
    local replace = patch.replace
    if type(find) ~= "string" or type(replace) ~= "string" then
      error("INVALID_ARGUMENT: each patch must have 'find' and 'replace' strings", 0)
    end

    local start_idx, end_idx = string.find(content, find, 1, true)
    if not start_idx then
      error("NOT_FOUND: Could not find exact match for: " .. find, 0)
    end

    local second_start = string.find(content, find, end_idx + 1, true)
    if second_start then
      error("FAILED_PRECONDITION: Multiple matches found for: " .. find ..
                ". Please use a more specific 'find' block.", 0)
    end

    content = string.sub(content, 1, start_idx - 1) .. replace ..
                  string.sub(content, end_idx + 1)
  end

  local f_out = io.open(args.path, "w")
  if not f_out then
    error("Could not open file for writing: " .. args.path, 0)
  end
  f_out:write(content)
  f_out:close()

  return "File written successfully: " .. args.path
end
