return function(args) {
  slop_guard();
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  if (!Array.isArray(args.patches)) throw new Error("INVALID_ARGUMENT: Missing mandatory field: patches");

  const path = args.path;
  let content = tools.read_file({path: path});

  for (let i = 0; i < args.patches.length; i++) {
    const patch = args.patches[i];
    if (!patch || typeof patch !== "object") {
      throw new Error("INVALID_ARGUMENT: patch at index " + i + " must be an object");
    }

    const find = patch.find;
    const replace = patch.replace;
    if (typeof find !== "string") {
      throw new Error("INVALID_ARGUMENT: patch.find at index " + i + " must be a string");
    }
    if (find.length === 0) {
      throw new Error("INVALID_ARGUMENT: patch.find at index " + i + " cannot be empty");
    }
    if (typeof replace !== "string") {
      throw new Error("INVALID_ARGUMENT: patch.replace at index " + i + " must be a string");
    }

    const idx = content.indexOf(find);
    if (idx === -1) {
      throw new Error("NOT_FOUND: Could not find exact match for the 'find' block at patch index " + i + " in: " + path);
    }

    const lastIdx = content.lastIndexOf(find);
    if (idx !== lastIdx) {
      throw new Error("FAILED_PRECONDITION: Ambiguous match for patch index " + i + ": multiple occurrences of the 'find' block in: " + path);
    }

    content = content.substring(0, idx) + replace + content.substring(idx + find.length);
  }

  tools.write_file({path: path, content: content});
  return "File patched successfully: " + path;
};

