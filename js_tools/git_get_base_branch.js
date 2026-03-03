return function(requested_base) {
  if (requested_base && requested_base !== "") return requested_base;
  
  const current = git_get_current_branch();
  if (!current) return "main";
  
  const res_json = tools.query_db({
    sql: "SELECT parent_branch FROM staging_branches WHERE branch_name = ?",
    params: [current]
  });
  
  if (res_json) {
    try {
      const rows = JSON.parse(res_json);
      if (rows && rows.length > 0 && rows[0].parent_branch) {
        return rows[0].parent_branch;
      }
    } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  
  if (current.startsWith("slop/staging/")) {
    throw new Error("Base branch not found in database for staging branch '" + current + "'.");
  }
  
  return "main";
};
