const parseToolRows = (typeof tools !== "undefined" && typeof tools.parse_tool_rows === "function")
  ? tools.parse_tool_rows
  : function parseToolRows(value, context) {
      if (Array.isArray(value)) return value;
      if (value == null || value === "") return [];
      if (typeof value === "string") {
        let parsed;
        try {
          parsed = JSON.parse(value);
        } catch (e) {
          throw new Error("Failed to parse " + context + ": " + e.message);
        }
        if (Array.isArray(parsed)) return parsed;
        throw new Error("Unexpected result shape for " + context);
      }
      if (typeof value === "object" && Array.isArray(value.rows)) return value.rows;
      throw new Error("Unexpected result shape for " + context);
    };

return function(args) {
  /**
   * Validate and normalize the requested skill name.
   *
   * @param {any} value
   * @returns {string}
   */
  function requireSkillName(value) {
    if (typeof value !== "string") throw new Error("INVALID_ARGUMENT: name must be a non-empty string");
    const trimmed = value.trim();
    if (trimmed === "") throw new Error("INVALID_ARGUMENT: name must be a non-empty string");
    return trimmed;
  }

  /**
   * Validate and normalize the requested action.
   *
   * @param {any} value
   * @returns {"activate"|"deactivate"}
   */
  function requireAction(value) {
    if (value !== "activate" && value !== "deactivate") {
      throw new Error("INVALID_ARGUMENT: action must be 'activate' or 'deactivate'");
    }
    return value;
  }

  if (!session_id || session_id === "") throw new Error("FAILED_PRECONDITION: No active session");

  const name = requireSkillName(args && args.name);
  const action = requireAction((args && args.action) || "activate");

  const sessionRows = parseToolRows(tools.query_db({
    sql: "SELECT active_skills FROM sessions WHERE id = ?",
    params: [session_id]
  }), "session lookup");
  if (sessionRows.length === 0) throw new Error("Session not found: " + session_id);

  const skillRows = parseToolRows(tools.query_db({
    sql: "SELECT name, system_prompt_patch FROM skills WHERE name = ?",
    params: [name]
  }), "skill lookup");
  if (skillRows.length === 0) throw new Error("UNKNOWN_SKILL: " + name);

  let skill_list = [];
  if (sessionRows[0].active_skills && sessionRows[0].active_skills !== "null") {
    try {
      const parsedSkills = JSON.parse(sessionRows[0].active_skills);
      if (Array.isArray(parsedSkills)) skill_list = parsedSkills;
    } catch (e) {
      throw new Error("Failed to parse active_skills: " + e.message);
    }
  }

  let prompt_patch = "";
  if (action === "activate") {
    if (!skill_list.includes(name)) {
      skill_list.push(name);
      tools.query_db({
        sql: "UPDATE skills SET activation_count = activation_count + 1 WHERE name = ?",
        params: [name]
      });
    }
    if (skillRows[0].system_prompt_patch) {
      prompt_patch = `

${skillRows[0].system_prompt_patch}`;
    }
  } else if (action === "deactivate") {
    skill_list = skill_list.filter(s => s !== name);
  }

  tools.query_db({
    sql: "UPDATE sessions SET active_skills = ? WHERE id = ?",
    params: [JSON.stringify(skill_list), session_id]
  });

  return "Skill '" + name + "' " + (action === "activate" ? "activated" : "deactivated") + "." + prompt_patch;
};


