return function(args) {
  const rows_json = tools.query_db({
    sql: "SELECT name, description, json_schema FROM js_functions"
  });
  const rows = JSON.parse(rows_json);

  const catalog = rows.map(row => ({
    name: row.name,
    description: row.description,
    json_schema: row.json_schema ? JSON.parse(row.json_schema) : {},
    source: "persisted"
  }));

  return catalog;
};

