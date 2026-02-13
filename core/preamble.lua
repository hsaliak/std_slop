-- ============================================================================
-- SLOP LUA ENVIRONMENT
-- ============================================================================
-- This environment provides direct access to the orchestrator state and tools.
-- 
-- GLOBALS:
--   session_id (string): The unique ID for the current interaction.
--   scratchpad (string): The current content of the persistent markdown scratchpad.
--   state      (string): The technical state summary (Goal/Context/Resolved).
--   history    (JSON):   The full conversation history as a list of message objects.
--   JSON       (table):  Utility for parsing/stringifying (JSON.parse, JSON.stringify).
--   tools      (table):  The registry of available tools.
--
-- You can modify 'scratchpad' or 'state' here and they will be persisted back 
-- to the orchestrator after the script execution finishes.
-- ============================================================================

-- Load the library logic
-- Note: ToolExecutor will ensure preamble_lib.lua is executed before this file
-- or that it is available in the package path.

if not tools then
  tools = {}
end

manifest = get_tool_manifest()
