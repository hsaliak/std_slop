return function(args) {
  if (!session_id || session_id === "") throw new Error("FAILED_PRECONDITION: No active session");
  const name = args.name;
  const action = args.action || "activate";
  
  const res = tools.query_db({
    sql: "SELECT active_skills FROM sessions WHERE id = ?",
    params: [session_id]
  });
  
  let rows = [];
  if (res) {
    try { rows = JSON.parse(res); } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  if (rows.length === 0) throw new Error("Session not found: " + session_id);
  
  let skill_list = [];
  if (rows[0].active_skills && rows[0].active_skills !== "null") {
    try { skill_list = JSON.parse(rows[0].active_skills); } catch (e) { print("Error parsing JSON from DB: " + e.message); }
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
    const res_skill = tools.query_db({
      sql: "SELECT system_prompt_patch FROM skills WHERE name = ?",
      params: [name]
    });
    if (res_skill) {
      try {
        const skill_rows = JSON.parse(res_skill);
        if (skill_rows.length > 0 && skill_rows[0].system_prompt_patch) {
          prompt_patch = "\n\n" + skill_rows[0].system_prompt_patch;
        }
      } catch (e) { print("Error parsing JSON from DB: " + e.message); }
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
