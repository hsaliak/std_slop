const parseToolRows = require("./parse_tool_rows.js");

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


