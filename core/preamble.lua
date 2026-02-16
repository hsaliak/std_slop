-- ============================================================================
-- SLOP LUA ENVIRONMENT
-- ============================================================================
-- This environment provides direct access to the orchestrator state and tools.
-- 
-- GLOBALS:
--   session_id (string): The unique ID for the current interaction.
--   scratchpad (table):  The persistent programmatic scratchpad.
--   state      (string): The technical state summary (Goal/Context/Resolved).
--   history    (JSON):   The full conversation history as a list of message objects.
--   JSON       (table):  Utility for parsing/stringifying (JSON.parse, JSON.stringify).
--   tools      (table):  The registry of available tools.
-- ============================================================================

-- Transform scratchpad from string to table if needed
if type(scratchpad) == "string" then
  local ok, parsed = pcall(JSON.parse, scratchpad)
  if ok and type(parsed) == "table" then
    scratchpad = parsed
  else
    scratchpad = { notes = scratchpad }
  end
elseif scratchpad == nil then
  scratchpad = {}
end

if state == nil then
  state = ""
end

if history == nil then
  history = {}
end

-- Load the library logic
-- Note: ToolExecutor will ensure preamble_lib.lua is executed before this file
if not tools then
  tools = {}
end
