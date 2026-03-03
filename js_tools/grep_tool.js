return function(args) {
  if (!args || typeof args.pattern !== "string") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }
  const simplified = {
    pattern: args.pattern,
    path: args.path || args.paths,
    context: args.context,
    limit: args.limit,
    include_ignored: args.include_ignored,
    ignore: args.ignore
  };
  return tools.grep(simplified);
};

