return function(args) {
  slop_guard();
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  if (typeof args.content !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: content");

  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  // Use a temporary file and mv to be safer, or just use printf/redirect
  // For simplicity, we'll use a heredoc-like approach with base64 to avoid escaping issues
  // But wait, we don't have base64 easily. Let's just use a simple redirect for now.
  // Actually, we can use a temporary file.
  const tmp_file = ".tmp_write_" + Math.random().toString(36).substring(7);
  
  // We'll use a more robust way in the future.
  const res = __os_run("cat > " + shell_escape(path) + " << 'EOF_SLOP'\n" + args.content + "\nEOF_SLOP\n");
  
  if (res.exit_code !== 0) {
    throw new Error("IO_ERROR: Failed to write to file: " + res.stderr);
  }

  return "File written successfully:\nPath: " + path + "\nBytes written: " + args.content.length + "\n";
};
