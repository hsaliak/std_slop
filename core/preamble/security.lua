function tools.help()
  return [[### Slop Orchestrator Help ###

#### Globals ####
- tools: Table containing all available tools.
- state: (string) Current technical state/progress.
- scratchpad: (table) Structured persistent storage across turns.
- history: (table) Array of conversation messages: {role, content}.

#### Core Tools ####
- help(): (string) Shows this help message.

#### File System & Expressive API ####
- tools.file(path): Returns a File object with read(), write(content), and patch({find, replace}).
- tools.files(glob): Returns a Collection of File objects; use :foreach(fn).
- list_directory({path=".", depth=1, git_only=false}): Lists directory contents.
- tools.grep({pattern, patterns, query, path, context, case_insensitive, word_regexp, ...}): Unified search using git grep or standard grep. Returns a Result object.
- Result Objects: Structured output from tools. Use :lines(), :json(), or :filter(pattern).
- query_db({sql, params}): Executes a SQLite query (returns Result).
- describe_db({}): Schema of local database.

#### Shell & Systems ####
- dispatch_async(tool_name, args): Runs any tool asynchronously. Returns a Job object.
- tools.concurrent({task1, task2, ...}): Executes tools in parallel.
- tools.wait_all(job1, job2, ...): Waits for multiple jobs.

#### Patch Workflow (Mail Model) ####
- git_branch_staging({name}): Creates a slop/staging/ branch.
- git_commit_patch({summary, rationale}): Commits current changes as an atomic patch.
- git_reroll_patch({index, summary, rationale}): Updates an existing patch in the series.
- git_format_patch_series({base_branch}): Summarizes the current patchset.
- git_verify_series({command, base_branch}): Verifies the entire series passes tests.
- git_finalize_series({target_branch}): Merges the series and cleans up.

### Codebase Navigation Hierarchy ###
- **Explore First:** You MUST use `grep_tool` as your primary method for locating function definitions, variables, classes, or keywords.
- **Extract Second:** Use `tools.file(path):read()` **ONLY** after you have used `grep_tool` to confirm the exact file path.
- **Never Guess:** Do not use `tools.file(path):read()` to "guess" where a symbol might be located.]]
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
