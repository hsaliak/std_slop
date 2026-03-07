return function(args) {
  const rows = tools.query_db({
    sql: "SELECT name, description, json_schema FROM js_functions"
  });

  const catalog = rows.map(row => ({
    name: row.name,
    description: row.description,
    json_schema: row.json_schema ? JSON.parse(row.json_schema) : {}
  }));

  return catalog;
};

