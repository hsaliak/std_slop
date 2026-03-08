return function(args) {
  // Enforce the existing safety gate semantics for this tool.
  slop_guard();

  // Normalize mode early; only boolean true activates dry-run behavior.
  const dryRun = !!(args && args.dry_run === true);
  const mode = dryRun ? "dry_run" : "apply";

  // Helper to build deterministic summary counts from per-patch diagnostics.
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

  // Helper to produce a normalized failure envelope.
  function failEnvelope(path, diags, firstError) {
    return {
      ok: false,
      mode,
      code: firstError.code,
      path: typeof path === "string" ? path : null,
      can_apply: false,
      summary: buildSummary(diags),
      patches: diags,
      error: {
        patch_index: firstError.patch_index,
        match_count: firstError.match_count,
        message: firstError.message
      }
    };
  }

  // Validate top-level args in a deterministic LLM-readable way.
  const topLevelDiags = [];
  if (!args || typeof args.path !== "string") {
    const err = {
      patch_index: -1,
      code: "INVALID_ARGUMENT",
      match_count: 0,
      message: "Missing mandatory field: path"
    };
    topLevelDiags.push(err);
    return failEnvelope(null, topLevelDiags, err);
  }
  if (!Array.isArray(args.patches)) {
    const err = {
      patch_index: -1,
      code: "INVALID_ARGUMENT",
      match_count: 0,
      message: "Missing mandatory field: patches"
    };
    topLevelDiags.push(err);
    return failEnvelope(args.path, topLevelDiags, err);
  }

  const path = args.path;
  let content = tools.read_file({ path: path });

  // Preflight pass: validate each patch and compute exact-match cardinality.
  const diagnostics = [];
  for (let i = 0; i < args.patches.length; i++) {
    const p = args.patches[i];
    if (!p || typeof p.find !== "string" || typeof p.replace !== "string") {
      diagnostics.push({
        patch_index: i,
        code: "INVALID_ARGUMENT",
        match_count: 0,
        message: "Each patch must be an object with string fields: find, replace"
      });
      continue;
    }

    const find = p.find;
    // Count non-overlapping exact occurrences deterministically.
    let matchCount = 0;
    let cursor = 0;
    while (true) {
      const idx = content.indexOf(find, cursor);
      if (idx === -1) break;
      matchCount += 1;
      cursor = idx + find.length;
      // Guard zero-length find to avoid infinite loops.
      if (find.length === 0) {
        break;
      }
    }

    if (find.length === 0) {
      diagnostics.push({
        patch_index: i,
        code: "INVALID_ARGUMENT",
        match_count: 0,
        message: "Patch field 'find' must be non-empty"
      });
    } else if (matchCount === 0) {
      diagnostics.push({
        patch_index: i,
        code: "NOT_FOUND",
        match_count: 0,
        message: "Exact match not found for patch"
      });
    } else if (matchCount > 1) {
      diagnostics.push({
        patch_index: i,
        code: "AMBIGUOUS",
        match_count: matchCount,
        message: "Ambiguous match for patch"
      });
    } else {
      diagnostics.push({
        patch_index: i,
        code: "OK",
        match_count: 1,
        message: "Patch is applicable"
      });
    }
  }

  // If any patch failed preflight, return a structured failure envelope.
  let firstFailure = null;
  for (let i = 0; i < diagnostics.length; i++) {
    if (diagnostics[i].code !== "OK") {
      firstFailure = diagnostics[i];
      break;
    }
  }
  if (firstFailure) {
    return failEnvelope(path, diagnostics, firstFailure);
  }

  // Dry-run returns all diagnostics and summary, never mutating file content.
  if (dryRun) {
    return {
      ok: true,
      mode,
      path,
      can_apply: true,
      summary: buildSummary(diagnostics),
      patches: diagnostics
    };
  }

  // Apply mode: reuse validated patch list and perform exact substitutions.
  for (let i = 0; i < args.patches.length; i++) {
    const p = args.patches[i];
    const idx = content.indexOf(p.find);
    content = content.substring(0, idx) + p.replace + content.substring(idx + p.find.length);
  }

  tools.write_file({ path: path, content: content });
  return {
    ok: true,
    mode,
    path,
    applied: args.patches.length,
    can_apply: true,
    summary: buildSummary(diagnostics),
    patches: diagnostics
  };
};

