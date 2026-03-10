return function(args) {
  // Enforce workspace safety constraints before mutating files.
  slop_guard();

  // Supported arguments for the in-process unified diff applier.
  const {
    path,
    unified_diff,
    dry_run = false,
    ignore_whitespace = true
  } = args || {};

  // Validate mandatory arguments early for deterministic behavior.
  if (typeof path !== "string" || path.length === 0) {
    return { ok: false, code: "INVALID_ARGUMENT", error: { message: "path is required" } };
  }
  if (typeof unified_diff !== "string" || unified_diff.length === 0) {
    return { ok: false, code: "INVALID_ARGUMENT", error: { message: "unified_diff is required" } };
  }

  // Normalize lines for matching. With ignore_whitespace=true, collapse
  // whitespace runs and trim so spacing differences do not block matches.
  const normalizeForMatch = (line) => {
    const s = String(line);
    if (!ignore_whitespace) return s;
    return s.replace(/\s+/g, " ").trim();
  };

  // Split text into lines while preserving trailing newline state.
  const splitLines = (text) => {
    const normalized = String(text).replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    const trailing_newline = normalized.endsWith("\n");
    const lines = normalized.split("\n");
    if (trailing_newline) lines.pop();
    return { lines, trailing_newline };
  };

  // Parse hunks from unified diff, preserving only semantic ops.
  // We intentionally ignore @@ line-number metadata and file headers.
  const parseUnifiedDiff = (diffText) => {
    const src = String(diffText).replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    const lines = src.split("\n");
    const hunks = [];
    let current = null;

    for (let i = 0; i < lines.length; i += 1) {
      const line = lines[i];

      if (line.startsWith("@@")) {
        if (current && current.ops.length > 0) hunks.push(current);
        current = { header: line, ops: [] };
        continue;
      }
      if (!current) continue;
      if (line === "\\ No newline at end of file") continue;
      if (line.length === 0) continue;

      const prefix = line[0];
      const body = line.slice(1);
      if (prefix === " " || prefix === "-" || prefix === "+") {
        current.ops.push({ type: prefix, text: body });
      }
    }

    if (current && current.ops.length > 0) hunks.push(current);
    return hunks;
  };

  // Find first contiguous match of old-side lines (context + removals).
  // Prefer scanning from cursor, then wrap once for resilience.
  const findHunkStart = (fileLines, oldLines, startAt) => {
    const n = fileLines.length;
    const m = oldLines.length;
    if (m === 0) return n; // Pure-addition hunk with no anchors.

    const matchesAt = (idx) => {
      if (idx + m > n) return false;
      for (let j = 0; j < m; j += 1) {
        if (normalizeForMatch(fileLines[idx + j]) !== normalizeForMatch(oldLines[j])) {
          return false;
        }
      }
      return true;
    };

    for (let i = Math.max(0, startAt); i <= n - m; i += 1) {
      if (matchesAt(i)) return i;
    }
    for (let i = 0; i < Math.max(0, startAt); i += 1) {
      if (matchesAt(i)) return i;
    }
    return -1;
  };

  const oldFromOps = (ops) => {
    const out = [];
    for (let i = 0; i < ops.length; i += 1) if (ops[i].type !== "+") out.push(ops[i].text);
    return out;
  };
  const newFromOps = (ops) => {
    const out = [];
    for (let i = 0; i < ops.length; i += 1) if (ops[i].type !== "-") out.push(ops[i].text);
    return out;
  };

  const hunks = parseUnifiedDiff(unified_diff);
  if (hunks.length === 0) {
    return { ok: false, path, code: "PATCH_PARSE_FAILED", error: { message: "No valid hunks found in unified_diff" } };
  }

  // Apply to in-memory copy so dry-run and real apply share the same logic.
  const originalText = tools.read_file({ path });
  const split = splitLines(originalText);
  const workingLines = split.lines.slice();
  let cursor = 0;

  for (let h = 0; h < hunks.length; h += 1) {
    const oldLines = oldFromOps(hunks[h].ops);
    const newLines = newFromOps(hunks[h].ops);

    const at = findHunkStart(workingLines, oldLines, cursor);
    if (at < 0) {
      return {
        ok: false,
        path,
        code: "PATCH_DRY_RUN_FAILED",
        error: { message: "Unable to match hunk in target file", detail: hunks[h].header, hunk_index: h }
      };
    }

    workingLines.splice(at, oldLines.length, ...newLines);
    cursor = at + newLines.length;
  }

  if (dry_run) {
    return { ok: true, mode: "dry_run", path, can_apply: true, applied: hunks.length, options: { ignore_whitespace } };
  }

  const finalText = workingLines.join("\n") + (split.trailing_newline ? "\n" : "");
  tools.write_file({ path, content: finalText });

  return { ok: true, mode: "apply", path, applied: hunks.length, options: { ignore_whitespace } };
};



