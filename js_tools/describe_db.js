return function(args) {
  const res = tools.query_db({sql: "SELECT name, sql FROM sqlite_master WHERE type='table'"});
  return res;
};
