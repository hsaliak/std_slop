const native_llm_query = tools.llm_query;
return function(args) {
  if (!args || !args.query) throw new Error("Usage: llm_query({query: '...'})");
  return native_llm_query(args);
};

