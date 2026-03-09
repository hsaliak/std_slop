return function(args) {
  slop_guard();
  
  const { path, unified_diff, dry_run, strip = 0, fuzz = 3, ignore_whitespace = true } = args;
  
  if (!unified_diff) {
    return { ok: false, error: "unified_diff is required" };
  }

  const shellQuote = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";
  const tmpPath = ".slop_patch_" + Date.now() + "_" + Math.floor(Math.random() * 1000000) + ".diff";
  
  try {
    tools.write_file({ path: tmpPath, content: unified_diff });

    const flags = [
      "--batch", 
      "--forward", 
      "-p" + strip, 
      "-F" + fuzz, 
      ignore_whitespace ? "-l" : "", 
      "--reject-file=-"
    ].filter(Boolean).join(" ");

    const runPatch = (isDry) => {
      let cmd = "patch " + (isDry ? "--dry-run " : "") + flags;
      if (path) {
        cmd += " " + shellQuote(path);
      }
      cmd += " < " + shellQuote(tmpPath);
      return tools.execute_bash({
        command: cmd,
        allow_nonzero_exit: true
      });
    };

    const dryRes = runPatch(true);
    if (dryRes.exit_code !== 0) {
      return { 
        ok: false, 
        path, 
        code: "PATCH_DRY_RUN_FAILED", 
        error: { 
          message: "Unified diff dry-run failed", 
          detail: (dryRes.stderr || "") + (dryRes.stdout || "") 
        } 
      };
    }

    if (dry_run) {
      return { ok: true, mode: "dry_run", path, can_apply: true };
    }

    const applyRes = runPatch(false);
    if (applyRes.exit_code !== 0) {
      return { 
        ok: false, 
        path, 
        code: "PATCH_APPLY_FAILED", 
        error: { 
          message: "Unified diff apply failed", 
          detail: (applyRes.stderr || "") + (applyRes.stdout || "") 
        } 
      };
    }

    return { ok: true, mode: "apply", path, applied: 1 };
  } finally {
    tools.execute_bash({ command: "rm -f " + shellQuote(tmpPath), allow_nonzero_exit: true });
  }
};

