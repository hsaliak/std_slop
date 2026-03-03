return function(args) {
  if (typeof args.path !== "string") throw new Error("INVALID_ARGUMENT: Missing mandatory field: path");
  const path = args.path;
  if (path.includes("..") || path.startsWith("/")) {
    throw new Error("SECURITY_VIOLATION: Path traversal (..) or absolute paths are not allowed.");
  }

  function parseOptionalInteger(value, fieldName) {
    if (value === undefined || value === null) return null;
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value === "string" && /^-?\d+$/.test(value.trim())) {
      return Number.parseInt(value, 10);
    }
    throw new Error("INVALID_ARGUMENT: " + fieldName + " must be an integer");
  }

  const parsed_start_line = parseOptionalInteger(args.start_line, "start_line");
  const parsed_end_line = parseOptionalInteger(args.end_line, "end_line");

  let cmd = "cat " + shell_escape(path);
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Could not open file: " + res.stderr);
  }

  let lines = res.stdout.split("\n");
  if (res.stdout.endsWith("\n")) lines.pop();

  const start_line = parsed_start_line === null ? 1 : parsed_start_line;
  const end_line = parsed_end_line === null ? lines.length : parsed_end_line;

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


