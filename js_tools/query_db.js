const native_query_db = tools.query_db;
return function(args) {
  if (!args || !args.sql) throw new Error("Usage: query_db({sql: '...'})");
  return native_query_db(args);
};

