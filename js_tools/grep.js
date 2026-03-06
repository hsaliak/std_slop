return function(args) {
  if (!args || typeof args.pattern !== "string" || args.pattern === "") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }

  const pattern = args.pattern;
  const searchPath = args.path || ".";

  function parseOptionalInteger(value, fallback) {
    if (value === undefined || value === null) return fallback;
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value === "string" && /^-?\d+$/.test(value.trim())) return parseInt(value.trim(), 10);
    return fallback;
  }

  function splitIgnoreInput(value) {
    if (value === undefined || value === null) return [];
    if (Array.isArray(value)) return value.filter(function(v) { return typeof v === "string" && v.trim() !== ""; });
    if (typeof value === "string") {
      return value.split(",").map(function(v) { return v.trim(); }).filter(function(v) { return v !== ""; });
    }
    return [];
  }

  function uniquePush(list, value) {
    if (value && list.indexOf(value) === -1) list.push(value);
  }

  function resolveRepoRoot() {
    const res = __os_run("git rev-parse --show-toplevel");
    if (res.exit_code !== 0) return null;
    const out = (res.stdout || "").trim();
    return out === "" ? null : out;
  }

  function parseRootGitignore(repoRoot) {
    const result = { dirs: [], files: [], skipped: 0 };
    if (!repoRoot) return result;

    const filePath = repoRoot + "/.gitignore";
    const check = __os_run("test -f " + shell_escape(filePath));
    if (check.exit_code !== 0) return result;

    const cat = __os_run("cat " + shell_escape(filePath));
    if (cat.exit_code !== 0) return result;

    const lines = (cat.stdout || "").split("\n");
    for (let i = 0; i < lines.length; i++) {
      const raw = lines[i];
      const line = raw.trim();
      if (line === "" || line[0] === "#") continue;

      if (line[0] === "!" || line.indexOf("**") !== -1) {
        result.skipped += 1;
        continue;
      }

      if (line[line.length - 1] === "/") {
        const dir = line.slice(0, -1).trim();
        if (dir === "" || dir.indexOf("/") !== -1) {
          result.skipped += 1;
          continue;
        }
        uniquePush(result.dirs, dir);
        continue;
      }

      if (line.indexOf("/") !== -1) {
        result.skipped += 1;
        continue;
      }

      uniquePush(result.files, line);
    }

    return result;
  }

  const limit = Math.max(1, parseOptionalInteger(args.limit, 200));
  const context = Math.max(0, parseOptionalInteger(args.context, 0));
  const contextArg = context > 0 ? " -C " + context : "";

  const defaultIgnores = [".git", "node_modules", "bazel-*", "dist", "build", ".cache", "target"];
  const userIgnores = splitIgnoreInput(args.ignore);
  const includeIgnored = args.include_ignored === true;

  const repoRoot = resolveRepoRoot();
  const gitignore = parseRootGitignore(repoRoot);

  const excludeDirs = [];
  const excludeFiles = [];

  if (!includeIgnored) {
    for (let i = 0; i < defaultIgnores.length; i++) uniquePush(excludeDirs, defaultIgnores[i]);
  }
  for (let i = 0; i < userIgnores.length; i++) uniquePush(excludeDirs, userIgnores[i]);
  if (!includeIgnored) {
    for (let i = 0; i < gitignore.dirs.length; i++) uniquePush(excludeDirs, gitignore.dirs[i]);
  }
  if (!includeIgnored) {
    for (let i = 0; i < gitignore.files.length; i++) uniquePush(excludeFiles, gitignore.files[i]);
  }

  const excludeArgs = [];
  for (let i = 0; i < excludeDirs.length; i++) excludeArgs.push("--exclude-dir=" + shell_escape(excludeDirs[i]));
  for (let i = 0; i < excludeFiles.length; i++) excludeArgs.push("--exclude=" + shell_escape(excludeFiles[i]));

  const modeFlag = args.fixed_strings === true ? "-F" : "-E";
  const cmd = "grep -rn " + modeFlag + " -I --color=never" +
    contextArg +
    (excludeArgs.length ? " " + excludeArgs.join(" ") : "") +
    " -e " + shell_escape(pattern) +
    " " + shell_escape(searchPath);

  const res = __os_run(cmd);
  if (res.exit_code !== 0 && res.exit_code !== 1) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + "\nOutput:\n" + (res.stdout || "") + (res.stderr || ""));
  }

  const stdout = res.stdout || "";
  const lines = stdout === "" ? [] : stdout.split("\n");
  let output = stdout;

  if (lines.length > limit) {
    output = lines.slice(0, limit).join("\n") +
      "\n[TRUNCATED: Use a more specific pattern or path to narrow results]";
  }

  if (gitignore.skipped > 0) {
    output = output + (output === "" ? "" : "\n") +
      "[NOTE: Skipped " + gitignore.skipped + " unsupported root .gitignore rule(s)]";
  }

  return output;
};


