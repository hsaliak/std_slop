return function(args) {
  if (!args || typeof args.pattern !== "string" || args.pattern === "") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }

  const pattern = args.pattern;
  const path = args.path || ".";

  function parseOptionalInteger(value, fallback) {
    if (value === undefined || value === null) return fallback;
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value === "string" && /^-?\d+$/.test(value.trim())) {
      return Number.parseInt(value, 10);
    }
    return fallback;
  }

  const parsedLimit = parseOptionalInteger(args.limit, 500);
  const limit = Number.isInteger(parsedLimit) && parsedLimit > 0 ? parsedLimit : 500;

  const parsedContext = parseOptionalInteger(args.context, null);
  const context = (parsedContext !== null && parsedContext >= 0) ? parsedContext : null;

  const includeIgnored = args.include_ignored === true;
  if (args.ignore !== undefined && !Array.isArray(args.ignore)) {
    throw new Error("INVALID_ARGUMENT: ignore must be an array of strings");
  }

  const defaultIgnores = [".git", "node_modules", "bazel-*", "dist", "build", ".cache", "target"];
  const customIgnore = Array.isArray(args.ignore)
    ? args.ignore.filter(p => typeof p === "string" && p.trim() !== "")
    : [];
  const ignorePatterns = includeIgnored ? [] : defaultIgnores.concat(customIgnore);

  let contextArg = "";
  if (context !== null) {
    contextArg = " -C " + context;
  }

  let excludeArgs = "";
  if (ignorePatterns.length > 0) {
    excludeArgs = " " + ignorePatterns.map(p => "--exclude-dir=" + shell_escape(p)).join(" ");
  }

  const cmd = "grep -rnE" + contextArg + excludeArgs + " -e " + shell_escape(pattern) + " " + shell_escape(path);
  const res = __os_run(cmd);
  if (res.exit_code !== 0 && res.exit_code !== 1) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + "\nOutput:\n" + res.stdout + res.stderr);
  }

  const lines = res.stdout === "" ? [] : res.stdout.split("\n");
  if (lines.length > limit) {
    return lines.slice(0, limit).join("\n") +
      "\n[TRUNCATED: Use a more specific pattern or path to narrow results]";
  }
  return res.stdout;
};

