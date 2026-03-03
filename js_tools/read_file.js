return function(args) {
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  if (args.start_line !== undefined && args.start_line !== null && typeof args.start_line !== "number") {
    throw new Error("INVALID_ARGUMENT: start_line must be an integer");
  }
  if (args.end_line !== undefined && args.end_line !== null && typeof args.end_line !== "number") {
    throw new Error("INVALID_ARGUMENT: end_line must be an integer");
  }

  let cmd = "cat " + shell_escape(path);
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Could not open file: " + res.stderr);
  }

  let lines = res.stdout.split("\n");
  if (res.stdout.endsWith("\n")) lines.pop();

  const start_line = args.start_line || 1;
  const end_line = args.end_line || lines.length;

  if (start_line > end_line) {
    throw new Error("INVALID_ARGUMENT: start_line (" + start_line + ") cannot be greater than end_line (" + end_line + ")");
  }

  if (start_line > lines.length) {
    return "";
  }

  let result_lines = lines.slice(Math.max(0, start_line - 1), end_line);
  if (args.line_numbers) {
    result_lines = result_lines.map((line, i) => (Math.max(1, start_line) + i) + ": " + line);
  }

  let body = result_lines.join("\n");
  if (body.length > 0) body += "\n";
  return body;
};
