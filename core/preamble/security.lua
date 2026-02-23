function tools.help()
  return [[### Slop Orchestrator Help ###

#### Workflow Constraints ####
- **Code Editing & File IO:** Prefer `tools.file(path):read()` and `tools.file(path):write(content)` for file operations. Use `tools.files(glob)` for batch operations.
- **Shell Commands:** Prefer Lua's native subprocess APIs (`os.execute`, `io.popen`) for running simple commands.

#### Globals ####
- tools: Table containing all available tools.
- scratchpad: (table) Structured persistent storage across turns.
- state: (table) Current operational context (active files, branch, goal).
- history: (table) Metadata about the conversation history.

#### Core Tools ####
- list_directory({path, depth}): Lists files and directories recursively.
- grep({path, pattern}): Unified search using git grep or standard grep. Returns a Result object.
- Result Objects: Structured output from tools. Use :lines(), :json(), or :filter(pattern).
- query_db({sql, params}): Executes a SQLite query (returns Result).
- describe_db({}): Schema of local database.
- use_skill({name, action="activate"}): Activates/Deactivates a persona/skill.
- manage_scratchpad({action, key, value}): Manages persistent storage. Actions: "read", "update", "delete", "clear".

#### File System ####
- tools.file(path): Returns a File object.
- tools.files(glob): Returns a Collection of File objects; use :foreach(fn).
- File Objects: Use :read() and :write(content).

#### Shell & Systems ####
- dispatch_async(tool_name, args): Runs any tool asynchronously. Returns a Job object.
- tools.concurrent({task1, task2, ...}): Executes tools in parallel.
- tools.wait_all(job1, job2, ...): Waits for multiple jobs.

#### Patch Workflow (Patcher) ####
- git_branch_staging({name}): Creates slop/staging/<name> and switches to it.
- git_commit_patch({summary, rationale}): Commits staged changes as a patch.
- git_reroll_patch({index}): Updates an existing patch in the series.
- git_verify_series({cmd}): Runs build/test on every patch in the series.
- git_format_patch_series({}): Formats the series for review.
- git_finalize_series({}): Merges the series and cleans up.

### Codebase Navigation Hierarchy ###
- **Explore First:** You MUST use `tools.grep` as your primary method for locating function definitions, variables, classes, or keywords.
- **Extract Second:** Use `tools.file(path):read()` **ONLY** after you have located the target via grep.
- **Context Hygiene:** Do NOT read large files (>500 lines) into context. Use `grep` or specific `sed` ranges.
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
