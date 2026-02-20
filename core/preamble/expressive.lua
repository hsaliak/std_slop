-- Expressive Lua Layer for Slop LCP

-- Utility: String Splitting
local function split_lines(s)
  local lines = {}
  for line in s:gmatch("([^\r\n]+)") do
    table.insert(lines, line)
  end
  return lines
end

-- Result Object Metatable
local Result = {}
Result.__index = Result

function Result:lines() return split_lines(self.raw) end
function Result:json() return JSON.parse(self.raw) end
function Result:__tostring() return self.raw end

-- File Object Metatable
local File = {}
File.__index = File

function tools.file(path)
  return setmetatable({ path = path }, File)
end

function File:read(args)
  args = args or {}
  args.path = self.path
  return tools.read_file(args)
end

function File:patch(args)
  args = args or {}
  args.path = self.path
  return tools.apply_patch(args)
end

function File:write(content)
  return tools.write_file({ path = self.path, content = content })
end

-- Concurrent Execution
function tools.concurrent(tasks)
  local jobs = {}
  for i, task in ipairs(tasks) do
    if type(task) == "string" then
      -- Shortcut: tools.concurrent({"ls", "pwd"})
      table.insert(jobs, tools.dispatch_async(task, {}))
    else
      table.insert(jobs, tools.dispatch_async(task.tool or task[1], task.args or task[2] or {}))
    end
  end
  
  local results = {}
  for i, job in ipairs(jobs) do
    results[i] = job:wait()
  end
  return results
end

-- Job Waiting
function tools.wait_all(...)
  local args = {...}
  local jobs = args
  if #args == 1 and type(args[1]) == "table" then
    jobs = args[1]
  end
  
  local results = {}
  for i, job in ipairs(jobs) do
    if type(job) == "userdata" and job.wait then
      results[i] = job:wait()
    else
      results[i] = job
    end
  end
  return results
end

-- Collection API
local Collection = {}
Collection.__index = Collection

function tools.files(pattern)
  local res = tools.execute_bash({command = "find . -maxdepth 2 -name '" .. pattern .. "'"})
  local paths = split_lines(res)
  local items = {}
  for _, p in ipairs(paths) do
    table.insert(items, tools.file(p))
  end
  return setmetatable(items, Collection)
end

function Collection:foreach(fn)
  for _, item in ipairs(self) do
    fn(item)
  end
end

-- Auto-wrap existing tools to return Result objects
local wrapped_tools = {
  "grep",
  "read_file", "execute_bash", "git_grep_tool", "query_db", "llm_query"
}

for _, name in ipairs(wrapped_tools) do
  local original = tools[name]
  if original then
    tools[name] = function(args)
      local res = original(args)
      if type(res) == "string" then
        return setmetatable({ raw = res }, Result)
      end
      return res
    end
  end
end

-- Auto-Memo System
local function auto_memo_inject(path)
  if not path then return end
  local tags = {path}
  local dir = path:match("(.*)/")
  if dir then table.insert(tags, dir) end
  
  -- Use pcall to avoid crashing if DB is busy or retrieve_memos fails
  pcall(function()
    local memos = tools.retrieve_memos({tags = tags})
    if type(memos) == "string" then
      memos = JSON.parse(memos)
    end
    
    if memos and #memos > 0 then
      local current = scratchpad.relevant_memos or {}
      local changed = false
      for _, memo in ipairs(memos) do
        if not current[memo.content] then
          current[memo.content] = memo.tags
          changed = true
        end
      end
      if changed then
        tools.manage_scratchpad({action = "update", key = "relevant_memos", value = current})
      end
    end
  end)
end

-- Hook into read_file and apply_patch
local base_read = tools.read_file
function tools.read_file(args)
  auto_memo_inject(args.path)
  return base_read(args)
end

local base_patch = tools.apply_patch
function tools.apply_patch(args)
  auto_memo_inject(args.path)
  return base_patch(args)
end
