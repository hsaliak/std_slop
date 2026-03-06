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

return function() {
  // Preserve original behavior: allow destructive tools in standard mode,
  // otherwise enforce staging branch protection.
  try {
    const rows = parseToolRows(
      tools.query_db({ sql: "SELECT mode FROM settings WHERE id = 1" }),
      "settings mode lookup"
    );
    if (rows.length > 0 && rows[0].mode === "standard") {
      return;
    }
  } catch (e) {
    // Keep fail-open semantics for DB lookup issues, then rely on branch guard.
  }

  const branch = git_get_current_branch();
  if (!branch) return;

  if (!branch.startsWith("slop/staging/") && branch !== "HEAD") {
    throw new Error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " + branch);
  }
};

