return function(args) {
  if (!args || typeof args.pattern !== "string" || args.pattern === "") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }

  const pattern = args.pattern;
  const path = args.path || ".";
  const context = args.context;
  const limit = args.limit ? parseInt(args.limit, 10) : 500;

  let context_arg = "";
  if (context !== undefined && context !== null) {
    const ctx = parseInt(context, 10);
    if (!Number.isNaN(ctx) && ctx >= 0) {
      context_arg = " -C " + ctx;
    }
  }

  const cmd = "grep -rnE" + context_arg + " -e " + shell_escape(pattern) + " " + shell_escape(path);
  const res = __os_run(cmd);
  if (res.exit_code !== 0 && res.exit_code !== 1) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + "\nOutput:\n" + res.stdout + res.stderr);
  }

  const lines = res.stdout.split("\n");
  if (lines.length > limit) {
    return lines.slice(0, limit).join("\n") +
      "\n[TRUNCATED: Use a more specific pattern or path to narrow results]";
  }
  return res.stdout;
};
