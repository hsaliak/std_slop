return function(args) {
  slop_guard();
  const dryRun = !!(args && args.dry_run === true);
  const mode = dryRun ? "dry_run" : "apply";

  function buildSummary(diags) {
    const summary = { total: diags.length, ok: 0, not_found: 0, ambiguous: 0, invalid: 0 };
    for (let i = 0; i < diags.length; i++) {
      const code = diags[i].code;
      if (code === "OK") summary.ok += 1;
      else if (code === "NOT_FOUND") summary.not_found += 1;
      else if (code === "AMBIGUOUS") summary.ambiguous += 1;
      else if (code === "INVALID_ARGUMENT") summary.invalid += 1;
    }
    return summary;
  }

  function failEnvelope(path, diags, firstError) {
    return {
      ok: false, mode, path, code: firstError.code, can_apply: false,
      summary: buildSummary(diags), patches: diags,
      error: { patch_index: firstError.patch_index, match_count: firstError.match_count, message: firstError.message }
    };
  }

  function failUnified(path, code, message, detail) {
    return { ok: false, mode, path, code, can_apply: false, error: { message, detail: detail || "" } };
  }

  function shellQuote(value) { return "'" + String(value).replace(/'/g, "'\\''") + "'"; }

  function parseUnifiedDiffPaths(diffText) {
    const paths = [];
    const lines = String(diffText || "").split("\n");
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      if (!(line.startsWith("--- ") || line.startsWith("+++ "))) continue;
      let p = line.substring(4).trim();
      if (!p || p === "/dev/null") continue;
      const tabIdx = p.indexOf("\t");
      if (tabIdx !== -1) p = p.substring(0, tabIdx);
      if (p.startsWith("a/")) p = p.substring(2);
      if (p.startsWith("b/")) p = p.substring(2);
      paths.push(p);
    }
    return paths;
  }

  function validateUnifiedDiffPaths(parsedPaths, expectedPath) {
    const seen = {}; const unique = [];
    for (let i = 0; i < parsedPaths.length; i++) { const p = parsedPaths[i]; if (!seen[p]) { seen[p] = true; unique.push(p); } }
    if (unique.length === 0) return { ok: false, code: "INVALID_ARGUMENT", message: "Unified diff has no file headers" };
    for (let i = 0; i < unique.length; i++) { const p = unique[i]; if (p.startsWith("/") || p.includes("..")) return { ok: false, code: "INVALID_ARGUMENT", message: "Unified diff contains unsafe path: " + p }; }
    if (unique.length !== 1 || unique[0] !== expectedPath) return { ok: false, code: "INVALID_ARGUMENT", message: "Unified diff must target exactly args.path" };
    return { ok: true };
  }

  const topLevelDiags = [];
  if (!args || typeof args.path !== "string") {
    const err = { patch_index: -1, code: "INVALID_ARGUMENT", match_count: 0, message: "Missing mandatory field: path" };
    topLevelDiags.push(err); return failEnvelope(null, topLevelDiags, err);
  }

  const hasUnifiedDiff = !!(args && typeof args.unified_diff === "string" && args.unified_diff.length > 0);
  const hasPatches = !!(args && Array.isArray(args.patches));
  if (!hasUnifiedDiff && !hasPatches) {
    const err = { patch_index: -1, code: "INVALID_ARGUMENT", match_count: 0, message: "Missing mandatory field: patches (or provide unified_diff)" };
    topLevelDiags.push(err); return failEnvelope(args.path, topLevelDiags, err);
  }

  const path = args.path;
  if (hasUnifiedDiff) {
    const parsedPaths = parseUnifiedDiffPaths(args.unified_diff);
    const validation = validateUnifiedDiffPaths(parsedPaths, path);
    if (!validation.ok) return failUnified(path, validation.code, validation.message, "path_validation_failed");
    const strip = Number.isInteger(args.strip) ? args.strip : 0;
    const fuzz = Number.isInteger(args.fuzz) ? args.fuzz : 3;
    const ignoreWhitespace = args.ignore_whitespace !== false;
    const forward = args.forward !== false;
    const batch = args.batch !== false;
    const tmpPath = ".slop_patch_" + Date.now() + "_" + Math.floor(Math.random() * 1000000) + ".diff";
    tools.write_file({ path: tmpPath, content: args.unified_diff });
    const flags = []; if (batch) flags.push("--batch"); if (forward) flags.push("--forward"); flags.push("-p" + strip); if (ignoreWhitespace) flags.push("-l"); flags.push("-F " + fuzz); flags.push("--reject-file=-");
    const baseFlags = flags.join(" " );
    const dryRes = tools.execute_bash({ command: "patch --dry-run " + baseFlags + " < " + shellQuote(tmpPath), allow_nonzero_exit: true });
    if (dryRes.exit_code !== 0) { tools.execute_bash({ command: "rm -f " + shellQuote(tmpPath), allow_nonzero_exit: true }); return failUnified(path, "PATCH_DRY_RUN_FAILED", "Unified diff dry-run failed", (dryRes.stderr || "") + "\n" + (dryRes.stdout || "")); }
    if (dryRun) { tools.execute_bash({ command: "rm -f " + shellQuote(tmpPath), allow_nonzero_exit: true }); return { ok: true, mode, path, can_apply: true, backend: "system-patch", checks: { dry_run_exit_code: dryRes.exit_code, strip, fuzz, ignore_whitespace: ignoreWhitespace, forward, batch } }; }
    const applyRes = tools.execute_bash({ command: "patch " + baseFlags + " < " + shellQuote(tmpPath), allow_nonzero_exit: true });
    tools.execute_bash({ command: "rm -f " + shellQuote(tmpPath), allow_nonzero_exit: true });
    if (applyRes.exit_code !== 0) return failUnified(path, "PATCH_APPLY_FAILED", "Unified diff apply failed", (applyRes.stderr || "") + "\n" + (applyRes.stdout || ""));
    return { ok: true, mode, path, applied: 1, can_apply: true, backend: "system-patch", checks: { dry_run_exit_code: dryRes.exit_code, apply_exit_code: applyRes.exit_code, strip, fuzz, ignore_whitespace: ignoreWhitespace, forward, batch } };
  }

  let content = tools.read_file({ path });
  const diagnostics = [];
  for (let i = 0; i < args.patches.length; i++) {
    const p = args.patches[i];
    if (!p || typeof p.find !== "string" || typeof p.replace !== "string") { diagnostics.push({ patch_index: i, code: "INVALID_ARGUMENT", match_count: 0, message: "Each patch must be an object with string fields: find, replace" }); continue; }
    const find = p.find;
    if (find.length === 0) { diagnostics.push({ patch_index: i, code: "INVALID_ARGUMENT", match_count: 0, message: "Patch field 'find' must be non-empty" }); continue; }
    let matchCount = 0; let cursor = 0;
    while (true) { const idx = content.indexOf(find, cursor); if (idx === -1) break; matchCount += 1; cursor = idx + find.length; }
    if (matchCount === 0) diagnostics.push({ patch_index: i, code: "NOT_FOUND", match_count: 0, message: "Exact match not found for patch" });
    else if (matchCount > 1) diagnostics.push({ patch_index: i, code: "AMBIGUOUS", match_count: matchCount, message: "Ambiguous match for patch" });
    else diagnostics.push({ patch_index: i, code: "OK", match_count: 1, message: "Patch is applicable" });
  }
  let firstFailure = null; for (let i = 0; i < diagnostics.length; i++) { if (diagnostics[i].code !== "OK") { firstFailure = diagnostics[i]; break; } }
  if (firstFailure) return failEnvelope(path, diagnostics, firstFailure);
  if (dryRun) return { ok: true, mode, path, can_apply: true, summary: buildSummary(diagnostics), patches: diagnostics };
  for (let i = 0; i < args.patches.length; i++) { const p = args.patches[i]; const idx = content.indexOf(p.find); content = content.substring(0, idx) + p.replace + content.substring(idx + p.find.length); }
  tools.write_file({ path, content });
  return { ok: true, mode, path, applied: args.patches.length, can_apply: true, summary: buildSummary(diagnostics), patches: diagnostics };
};
