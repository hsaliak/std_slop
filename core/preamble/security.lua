function tools.help()
  return [[### Slop Orchestrator Help ###

#### Workflow Constraints ####
- **Code Editing & File IO:** Use `tools.read_file({path})` and `tools.write_file({path, content})`.
- **Shell Commands:** Use `tools.execute_bash({command = "..."})`.

#### Globals ####
- tools: Table containing all available tools.
- scratchpad: (table) Structured persistent storage across turns.
- state: (table) Current operational context.

#### Core Tools ####
- read_file({path}): Reads file content.
- write_file({path, content}): Writes file content (guarded).
- list_directory({path, depth}): Lists files and directories.
- grep({path, pattern}): Unified search.
- execute_bash({command}): Executes a bash command (guarded).
- execute_bash_async({command}): Executes a bash command asynchronously.
- apply_patch({patch}): Applies a unified diff.
- manage_scratchpad({action, key, value}): Manages persistent storage.
- query_db({sql, params}): Executes a SQLite query.
- describe_db({}): Schema of local database.
- use_skill({name, action}): Activates/Deactivates a persona/skill.
- persist_function({name, code, test_args, expected_result}): Saves a Lua function.
- dispatch_async(tool_name, args): Runs a tool asynchronously.
- ask_user({prompt}): Prompts the user for input.
- help(): Displays this help.
]]
end

-- Security Wrappers for Standard Library (Mail Mode Protection)
-- ============================================================================
-- These wrappers invoke slop_guard() to ensure that destructive operations 
-- are only performed in authorized contexts (e.g., staging branches).
-- In 'standard' mode, slop_guard() returns immediately, preserving 
-- full expressiveness.

local _native_os_execute = os.execute
os.execute = function(...)
  slop_guard()
  return _native_os_execute(...)
end

local _native_io_popen = io.popen
io.popen = function(...)
  slop_guard()
  return _native_io_popen(...)
end

local _native_os_exit = os.exit
os.exit = function(...)
  slop_guard()
  return _native_os_exit(...)
end

local _native_os_remove = os.remove
os.remove = function(...)
  slop_guard()
  return _native_os_remove(...)
end

local _native_os_rename = os.rename
os.rename = function(...)
  slop_guard()
  return _native_os_rename(...)
end

local _native_os_tmpname = os.tmpname
os.tmpname = function(...)
  slop_guard()
  return _native_os_tmpname(...)
end

local _native_io_tmpfile = io.tmpfile
io.tmpfile = function(...)
  slop_guard()
  return _native_io_tmpfile(...)
end

local _native_io_output = io.output
io.output = function(...)
  slop_guard()
  return _native_io_output(...)
end

local _native_dofile = dofile
dofile = function(...)
  slop_guard()
  return _native_dofile(...)
end

local _native_loadfile = loadfile
loadfile = function(...)
  slop_guard()
  return _native_loadfile(...)
end

local _native_package_loadlib = package.loadlib
package.loadlib = function(...)
  slop_guard()
  return _native_package_loadlib(...)
end

local _native_io_open = io.open
io.open = function(path, mode)
  mode = mode or "r"
  -- Guard if attempting to write, append, or update
  if mode:find("[wa%+]") then
    slop_guard()
  end
  return _native_io_open(path, mode)
end

manifest = get_tool_manifest()

---