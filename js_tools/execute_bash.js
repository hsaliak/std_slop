return function(args) {
  try {
    slop_guard();
  } catch (e) {
    throw new Error(
        "Branch safety gate blocked execute_bash before command execution. " +
        "This does not imply previous tool steps failed. Original error: " +
        e.message);
  }

  if (!args || typeof args !== "object") {
    throw new Error("Invalid arguments: expected an object");
  }

  const command = args.command;
  if (typeof command !== "string") {
    throw new Error("Invalid arguments: command is required and must be a string");
  }

  const cwd = args.cwd;
  if (cwd !== undefined && typeof cwd !== "string") {
    throw new Error("Invalid arguments: cwd must be a string when provided");
  }

  const allow_nonzero_exit = args.allow_nonzero_exit === true;

  const env = args.env;
  if (env !== undefined) {
    if (!env || typeof env !== "object" || Array.isArray(env)) {
      throw new Error("Invalid arguments: env must be an object when provided");
    }
    for (const key in env) {
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) {
        throw new Error("Invalid arguments: env key is not a valid shell identifier: " + key);
      }
      if (typeof env[key] !== "string") {
        throw new Error("Invalid arguments: env values must be strings: " + key);
      }
    }
  }

  function build_command() {
    const parts = [];
    if (env !== undefined) {
      const keys = Object.keys(env).sort();
      for (let i = 0; i < keys.length; ++i) {
        const key = keys[i];
        parts.push(key + "=" + shell_escape(env[key]));
      }
    }
    if (cwd !== undefined) {
      parts.push("cd " + shell_escape(cwd) + " &&");
    }
    parts.push(command);
    return parts.join(" " );
  }

  function run(command_to_run) {
    const res = __os_run(command_to_run);
    return {
      res: res,
      stdout: res.stdout || "",
      stderr: res.stderr || "",
      out: (res.stdout || "") + (res.stderr || "")
    };
  }

  function has_any(text, needles) {
    for (let i = 0; i < needles.length; ++i) {
      if (text.indexOf(needles[i]) !== -1) {
        return true;
      }
    }
    return false;
  }

  function tokenize_shellish(text) {
    return text.split(/\s+/).filter(function(token) {
      return token !== "";
    });
  }

  function has_standalone_token(command_text, token) {
    const tokens = tokenize_shellish(command_text);
    for (let i = 0; i < tokens.length; ++i) {
      if (tokens[i] === token) {
        return true;
      }
    }
    return false;
  }

  function maybe_regex_intent(command_text) {
    return /(grep|egrep|sed|awk|find)\b/.test(command_text);
  }

  function collect_hints(command_text, stdout_text, stderr_text) {
    const combined = stdout_text + "\n" + stderr_text;
    const hints = [];

    if (has_any(combined, [
          "syntax error near unexpected token `('",
          "syntax error near unexpected token '('",
          "syntax error near unexpected token `)'",
          "syntax error near unexpected token ')'"
        ]) || has_standalone_token(command_text, "(") ||
        has_standalone_token(command_text, ")")) {
      hints.push("This may be a JavaScript-to-shell escaping issue. In JavaScript string literals, write \\( and \\) so the shell receives \( and \).");
    }

    if (has_any(combined, [
          "unexpected EOF while looking for matching",
          "unterminated quoted string",
          "Unterminated quoted string"
        ])) {
      hints.push("This looks like a shell quoting problem. Remember there are two layers: JavaScript string parsing happens before shell parsing.");
    }

    if (maybe_regex_intent(command_text) &&
        (command_text.indexOf("|") !== -1 || command_text.indexOf("+") !== -1 ||
         command_text.indexOf("?") !== -1 || command_text.indexOf("{") !== -1 ||
         command_text.indexOf("}") !== -1)) {
      hints.push("If you intended regex escapes such as \\|, \\+, \\?, \\{, or \\}, JavaScript string literals usually need doubled backslashes, for example \\\\| and \\\\+.");
    }

    if (has_any(combined, ["command not found", "not found"]) && /[|&;()]/.test(command_text)) {
      hints.push("A shell metacharacter may have been interpreted differently than intended. Re-check pipes, grouping, quoting, and escaping.");
    }

    return hints;
  }

  function format_failure(command_to_run, attempt) {
    const lines = [];
    lines.push("INTERNAL: Command failed with status " + attempt.res.exit_code);
    lines.push("Command:");
    lines.push(command_to_run);
    if (attempt.stdout !== "") {
      lines.push("");
      lines.push("Stdout:");
      lines.push(attempt.stdout);
    }
    if (attempt.stderr !== "") {
      lines.push("");
      lines.push("Stderr:");
      lines.push(attempt.stderr);
    }
    const hints = collect_hints(command, attempt.stdout, attempt.stderr);
    if (hints.length > 0) {
      lines.push("");
      lines.push("Hint:");
      for (let i = 0; i < hints.length; ++i) {
        lines.push("- " + hints[i]);
      }
    }
    return lines.join("\n");
  }

  const command_to_run = build_command();
  const attempt = run(command_to_run);
  if (attempt.res.exit_code !== 0 && !allow_nonzero_exit) {
    throw new Error(format_failure(command_to_run, attempt));
  }

  return {
    stdout: attempt.stdout,
    stderr: attempt.stderr,
    exit_code: attempt.res.exit_code,
    exitCode: attempt.res.exit_code,
    output: attempt.out,
    command: command,
    executed_command: command_to_run,
    toString: function() { return attempt.out; }
  };
};

