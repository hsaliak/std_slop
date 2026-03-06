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

return function(requested_base) {
  if (requested_base && requested_base !== "") return requested_base;
  
  const current = git_get_current_branch();
  if (!current) return "main";
  
  const rows = parseToolRows(tools.query_db({
    sql: "SELECT parent_branch FROM staging_branches WHERE branch_name = ?",
    params: [current]
  }), "staging branch lookup");
  if (rows.length > 0 && rows[0].parent_branch) {
    return rows[0].parent_branch;
  }
  
  if (current.startsWith("slop/staging/")) {
    throw new Error("Base branch not found in database for staging branch '" + current + "'.");
  }
  
  return "main";
};


