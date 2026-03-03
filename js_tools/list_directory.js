return function(args) {
  args = args || {};
  const path = args.path || ".";

  function parseOptionalInteger(value, fallback) {
    if (value === undefined || value === null) return fallback;
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value === "string" && /^-?\d+$/.test(value.trim())) {
      return Number.parseInt(value, 10);
    }
    return fallback;
  }

  const rawDepth = parseOptionalInteger(args.depth, 1);
  const depth = Math.max(1, Math.min(8, rawDepth));

  const includeIgnored = args.include_ignored === true;
  if (args.ignore !== undefined && !Array.isArray(args.ignore)) {
    throw new Error("INVALID_ARGUMENT: ignore must be an array of strings");
  }

  const defaultIgnores = [".git", "node_modules", "bazel-*", "dist", "build", ".cache", ".next", "target"];
  const customIgnore = Array.isArray(args.ignore)
    ? args.ignore.filter(p => typeof p === "string" && p.trim() !== "")
    : [];

  const ignorePatterns = includeIgnored ? [] : defaultIgnores.concat(customIgnore);

  let pruneClause = "";
  if (ignorePatterns.length > 0) {
    pruneClause = " \\( " + ignorePatterns.map(p => "-name " + shell_escape(p)).join(" -o ") + " \\) -prune -o";
  }

  const cmd =
    "find " + shell_escape(path) +
    " -mindepth 1 -maxdepth " + depth +
    pruneClause +
    " -mindepth 1 -maxdepth " + depth +
    " -printf '%y\t%P\n'";

  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Failed to list directory: " + res.stderr);
  }

  const lines = res.stdout.split("\n").filter(l => l.trim() !== "");
  const output = [];

  for (const line of lines) {
    const tabIdx = line.indexOf("\t");
    if (tabIdx === -1) continue;

    const type = line.substring(0, tabIdx);
    const rel = line.substring(tabIdx + 1);
    if (!rel) continue;

    if (type === "d") {
      output.push("Directory: " + rel + "/");
    } else {
      output.push("File: " + rel);
    }
  }

  return output.join("\n");
};

