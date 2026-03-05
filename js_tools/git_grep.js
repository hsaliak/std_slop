return function(args) {
  if (!args || typeof args.pattern !== "string" || args.pattern.trim() === "") {
    throw new Error("INVALID_ARGUMENT: Missing mandatory field: pattern");
  }

  const pattern = args.pattern;
  const format = (args.format === undefined || args.format === null) ? "structured" : String(args.format);
  if (format !== "structured" && format !== "raw") {
    throw new Error("INVALID_ARGUMENT: format must be 'structured' or 'raw'");
  }

  if (args.paths !== undefined) {
    if (!Array.isArray(args.paths)) {
      throw new Error("INVALID_ARGUMENT: paths must be an array of strings");
    }
    for (const p of args.paths) {
      if (typeof p !== "string" || p.trim() === "") {
        throw new Error("INVALID_ARGUMENT: paths must be an array of non-empty strings");
      }
    }
  }

  if (args.cwd !== undefined && (typeof args.cwd !== "string" || args.cwd.trim() === "")) {
    throw new Error("INVALID_ARGUMENT: cwd must be a non-empty string when provided");
  }

  const cwdPrefix = (typeof args.cwd === "string")
    ? "cd " + shell_escape(args.cwd) + " && "
    : "";

  const gitVersion = __os_run(cwdPrefix + "git --version");
  if (gitVersion.exit_code !== 0) {
    throw new Error("INTERNAL: git binary is not available");
  }

  const repoProbe = __os_run(cwdPrefix + "git rev-parse --is-inside-work-tree");
  const inRepo = repoProbe.exit_code === 0 && repoProbe.stdout.trim() === "true";
  const mode = inRepo ? "repo" : "no-index";

  const argv = ["git", "grep", "-n", "-I"];
  if (!inRepo) argv.push("--no-index");
  if (args.fixed === true) argv.push("-F");
  if (args.functionContext === true) argv.push("-W");
  if (args.ignoreCase === true) argv.push("-i");
  argv.push("-e", pattern);

  const paths = Array.isArray(args.paths) ? args.paths : [];
  if (paths.length > 0) {
    argv.push("--");
    for (const p of paths) argv.push(p);
  }

  const cmd = cwdPrefix + argv.map(shell_escape).join(" ");
  const res = __os_run(cmd);

  if (res.exit_code !== 0 && res.exit_code !== 1) {
    throw new Error(
      "INTERNAL: git grep failed with status " + res.exit_code + "\nOutput:\n" + res.stdout + res.stderr
    );
  }

  if (format === "raw") {
    return {
      ok: true,
      mode: mode,
      exitCode: res.exit_code,
      format: "raw",
      data: res.stdout
    };
  }

  const lines = res.stdout === "" ? [] : res.stdout.split("\n").filter(Boolean);
  const matches = [];
  for (const line of lines) {
    const m = line.match(/^(.*?):(\d+):(.*)$/);
    if (!m) continue;
    matches.push({
      path: m[1],
      line: Number.parseInt(m[2], 10),
      text: m[3]
    });
  }

  return {
    ok: true,
    mode: mode,
    exitCode: res.exit_code,
    format: "structured",
    data: matches
  };
};


