return function(args) {
  const res = tools.query_db({sql: "SELECT name, description, json_schema FROM tools WHERE is_enabled = 1"});
  return res;
};
