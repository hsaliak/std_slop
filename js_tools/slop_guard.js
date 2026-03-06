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

  const sessionId = args && args.session_id ? args.session_id : session_id;
  if (!sessionId) throw new Error("FAILED_PRECONDITION: No active session");

  const rows = parseToolRows(tools.query_db({
    sql: "SELECT mode FROM sessions WHERE id = ?",
    params: [sessionId]
  }), "session mode lookup");
  if (rows.length === 0) throw new Error("Session not found: " + sessionId);
  if (rows[0].mode !== "standard") {
    throw new Error("FAILED_PRECONDITION: Tool unavailable outside standard mode");
  }

  return true;
};



