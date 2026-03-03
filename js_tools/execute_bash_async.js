return function(args) {
  slop_guard();
  if (!args.command) throw new Error("Usage: execute_bash_async({command = '...'})");
  return tools.dispatch_async("execute_bash", args);
};
