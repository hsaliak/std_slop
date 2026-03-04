return function(args) {
  try {
    slop_guard();
  } catch (e) {
    throw new Error("Branch safety gate blocked execute_bash before command execution. This does not imply previous tool steps failed. Original error: " + e.message);
  }
  if (!args.command) throw new Error("Usage: execute_bash({command = '...'})");
  const res = __os_run(args.command);
  const out = res.stdout + res.stderr;
  if (res.exit_code !== 0) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + (out ? "\nOutput:\n" + out : ""));
  }
  const obj = new String(out);
  obj.stdout = res.stdout;
  obj.stderr = res.stderr;
  obj.exit_code = res.exit_code;
  obj.exitCode = res.exit_code;
  obj.output = out;
  obj.toJSON = function() {
    return {
      stdout: this.stdout,
      stderr: this.stderr,
      exit_code: this.exit_code,
      exitCode: this.exitCode,
      output: this.output
    };
  };
  return obj;
};

