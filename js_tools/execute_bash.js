return function(args) {
  try {
    slop_guard();
  } catch (e) {
    throw new Error("Branch safety gate blocked execute_bash before command execution. This does not imply previous tool steps failed. Original error: " + e.message);
  }

  const command = args && args.command;
  if (typeof command !== "string") {
    throw new Error("Invalid arguments: command is required and must be a string");
  }

  const res = __os_run(command);
  const out = res.stdout + res.stderr;

  if (res.exit_code !== 0) {
    throw new Error("INTERNAL: Command failed with status " + res.exit_code + (out ? "\nOutput:\n" + out : ""));
  }

  return {
    stdout: res.stdout,
    stderr: res.stderr,
    exit_code: res.exit_code,
    exitCode: res.exit_code,
    output: out
  };
};

