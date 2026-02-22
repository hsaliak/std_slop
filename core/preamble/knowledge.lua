function tools.manage_scratchpad(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session", 0) end
  local action = args.action
  
  if action == "read" then
    return scratchpad
  elseif action == "update" then
    if args.key then
      scratchpad[args.key] = args.value
    elseif args.content then
      scratchpad.notes = args.content
    elseif type(args.value) == "table" then
      for k, v in pairs(args.value) do scratchpad[k] = v end
    end
    
    -- Immediate Persistence
    local json_str = JSON.stringify(scratchpad)
    tools.query_db({
      sql = "UPDATE sessions SET scratchpad = ? WHERE id = ?",
      params = {json_str, session_id}
    })
    return "Scratchpad updated and persisted."
  elseif action == "append" then
    local current = scratchpad.notes or ""
    scratchpad.notes = current .. (args.content or "")
    
    local json_str = JSON.stringify(scratchpad)
    tools.query_db({
      sql = "UPDATE sessions SET scratchpad = ? WHERE id = ?",
      params = {json_str, session_id}
    })
    return "Scratchpad appended and persisted."
  else
    error("Unknown action: " .. tostring(action))
  end
end
function tools.describe_db(args)
  local query = "SELECT name, sql FROM sqlite_master WHERE type='table'"
  local success, res = call_tool(tools.query_db, {sql = query})
  if not success then error("Failed to describe database: " .. tostring(res)) end
  return res
end
function tools.use_skill(args)
  if not session_id or session_id == "" then error("FAILED_PRECONDITION: No active session") end
  local name = args.name
  local action = args.action or "activate"
  
  -- 1. Fetch current active_skills from DB
  local ok, res = call_tool(tools.query_db, {
    sql = "SELECT active_skills FROM sessions WHERE id = ?",
    params = {session_id}
  })
  if not ok then error("Failed to fetch skills: " .. tostring(res)) end
  
  local rows = JSON.parse(res)
  if #rows == 0 then error("Session not found: " .. session_id) end
  
  local skill_list_json = rows[1].active_skills
  local skill_list = {}
  if skill_list_json and skill_list_json ~= "" and skill_list_json ~= "null" then
    skill_list = JSON.parse(skill_list_json)
  end
  local skill_map = {}
  for _, s in ipairs(skill_list) do skill_map[s] = true end
  local prompt_patch = ""
  if action == "activate" then
    if not skill_map[name] then
      table.insert(skill_list, name)
      -- Increment activation count in meta-table
      call_tool(tools.query_db, {
        sql = "UPDATE skills SET activation_count = activation_count + 1 WHERE name = ?",
        params = {name}
      })
    end
    -- Get system prompt patch
    local ok_skill, res_skill = call_tool(tools.query_db, {
      sql = "SELECT system_prompt_patch FROM skills WHERE name = ?",
      params = {name}
    })
    if ok_skill then
      local skill_rows = JSON.parse(res_skill)
      if #skill_rows > 0 then
        prompt_patch = "\n\n" .. (skill_rows[1].system_prompt_patch or "")
      end
    end
  elseif action == "deactivate" then
    local new_list = {}
    for _, s in ipairs(skill_list) do
      if s ~= name then table.insert(new_list, s) end
    end
    skill_list = new_list
  end
  -- 2. Persist back to session
  local ok2, res2 = call_tool(tools.query_db, {
    sql = "UPDATE sessions SET active_skills = ? WHERE id = ?",
    params = {JSON.stringify(skill_list), session_id}
  })
  if not ok2 then error("Failed to update active skills: " .. tostring(res2)) end
  return "Skill '" .. name .. "' " .. (action == "activate" and "activated" or "deactivated") .. "." .. prompt_patch
end
-- Search Tools
