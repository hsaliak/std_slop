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
- read_file({path, start_line, end_line}): Reads a file (returns Result) (optional range).
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
- use_skill({name, action="activate"|"deactivate"}): Activates or deactivates specific system behaviors or prompt patches.

#### Search ####
- query_db({sql, params}): Executes a SQLite query on the project database.
- describe_db({}): Returns the schema of all tables in the local database.
- git_grep_tool({pattern, patterns, path, context, case_insensitive, word_regexp, ...}): Advanced git-grep. The use of this tool is *strongly preferred* over grep and read_file whenever possible.
- grep_tool({pattern, path, context}): Simple regex search.
- search_code({query}): Shortcut for searching code across the project.

#### Patch Workflow (Mail Model) ####
- git_branch_staging({name}): Creates a slop/staging/ branch.
- git_commit_patch({summary, rationale}): Commits current changes as an atomic patch.
- git_reroll_patch({index, summary, rationale}): Updates an existing patch in the series.
- git_format_patch_series({base_branch}): Summarizes the current patchset.
- git_verify_series({command, base_branch}): Verifies the entire series passes tests.
- git_finalize_series({target_branch}): Merges the series and cleans up.

#### Expressive Layer (Fluent API) ####
- tools.file(path): Returns a File object.
    - :read({start_line, end_line}): Reads the file.
    - :patch({patches}): Applies patches.
    - :write(content): Overwrites the file.
- tools.files(pattern): Returns a Collection of File objects matching the pattern.
    - :foreach(fn): Iterates over each file in the collection.
- tools.concurrent({task1, task2, ...}): Runs multiple tools in parallel and returns an array of results.
    Tasks can be strings ("ls") or tables {tool="name", args={...}}.
- tools.wait_all(job1, job2, ...): Waits for multiple jobs or values to complete.
- Result Objects: read_file, execute_bash, git_grep_tool, query_db, and llm_query now return Result objects.
    - :lines(): Returns an array of strings (one per line).
    - :json(): Parses the result as JSON.
- Auto-Memo Injection: Reading or patching a file automatically loads relevant architectural memos into `scratchpad.relevant_memos`.

### Codebase Navigation Hierarchy ###
- **Explore First:** You MUST use `git_grep_tool` as your primary method for locating function definitions, variables, classes, or keywords. This keeps the context window lean and isolated.
- **Extract Second:** Use `read_file` **ONLY** after you have used `git_grep_tool` to confirm the exact file path, and ONLY if you need the broader context of the surrounding code to complete the task.
- **Never Guess:** Do not use `read_file` to "guess" where a symbol might be located.
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
