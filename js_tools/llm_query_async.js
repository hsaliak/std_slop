return function(args) {
  if (!args.query) throw new Error("Usage: llm_query_async({query: '...'})");
  return tools.dispatch_async("llm_query", args);
};

