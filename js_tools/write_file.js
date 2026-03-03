return function(args) {
  slop_guard();
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  if (typeof args.content !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: content");

  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  const chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
  function randomSuffix(len) {
    let out = "";
    for (let i = 0; i < len; i++) {
      out += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return out;
  }

  let delimiter = "EOF_SLOP_" + randomSuffix(16);
  while (args.content.includes("\n" + delimiter + "\n") || args.content.endsWith("\n" + delimiter)) {
    delimiter = "EOF_SLOP_" + randomSuffix(16);
  }

  const cmd = "cat > " + shell_escape(path) + " << '" + delimiter + "'\n" + args.content + "\n" + delimiter + "\n";
  const res = __os_run(cmd);

  if (res.exit_code !== 0) {
    throw new Error("IO_ERROR: Failed to write to file: " + res.stderr);
  }

  return "File written successfully:\nPath: " + path + "\nBytes written: " + args.content.length + "\n";
};

