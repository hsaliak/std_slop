function tools.help()
  return [[
### Slop Orchestrator Help ###

#### Globals ####
- tools: Table containing all available tools.
- state: (string) Current technical state/progress.
- scratchpad: (table) Structured persistent storage across turns.
- history: (table) Array of conversation messages: {role, content}.

#### Core Tools ####
- llm_query({query, context}): (string) Runs a sub-task LLM query. 
    Accepts string or table: { query = "instruction", context = "data" | {"data1", "data2"} }
- llm_query_async({query, context}): (job) Asynchronous version of llm_query.
- help(): (string) Shows this help message.

#### File System ####
- read_file({path, start_line, end_line}): Reads a file (optional range).
- write_file({path, content}): Overwrites a file with new content.
- list_directory({path=".", depth=1, git_only=false}): Lists directory contents.
- apply_patch({path, patches}): Multi-replacement in a file. patches = {{find="...", replace="..."}}.

#### Shell & Execution ####
- execute_bash({command, input}): Executes a bash command synchronously.
- execute_bash_async({command, input}): Returns a Job object.
- dispatch_async(tool_name, args): Runs any tool asynchronously. Returns a Job object.

#### Knowledge Management ####
- save_memo({content, tags}): Saves a project invariant or learned convention.
- retrieve_memos({tags}): Searches for memos matching tags.
- manage_scratchpad({action="read"|"update", key, value, content}): Persistent memory.

#### Search ####
- query_db({sql, params}): Executes a SQLite query on the project database.
- describe_db({}): Returns the schema of all tables in the local database.
- git_grep_tool({pattern, patterns, path, context, case_insensitive, word_regexp, ...}): Advanced git-grep. The use of this tool is *strongly preferred* over grep and read_file whnevever possible.
- grep_tool({pattern, path, context}): Simple regex search.
- search_code({query}): Shortcut for searching code across the project.

#### Patch Workflow (Mail Model) ####
- git_branch_staging({name}): Creates a slop/staging/ branch.
- git_commit_patch({summary, rationale}): Commits current changes as an atomic patch.
- git_reroll_patch({index, summary, rationale}): Updates an existing patch in the series.
- git_format_patch_series({base_branch}): Summarizes the current patchset.
- git_verify_series({command, base_branch}): Verifies the entire series passes tests.
- git_finalize_series({target_branch}): Merges the series and cleans up.
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
