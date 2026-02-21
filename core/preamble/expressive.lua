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

