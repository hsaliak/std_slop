return function(args) {
  slop_guard();
  const summary = args.summary;
  const rationale = args.rationale;
  
  if (!summary) throw new Error("Summary is required");
  if (summary.length > 50) throw new Error("Summary must be <= 50 characters");
  
  const full_msg = summary + "\n\n" + (rationale || "");
  const cmd = `git commit -m ${shell_escape(full_msg)}`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) throw new Error("Commit failed: " + res.stdout + res.stderr);
  
  return tools.git_format_patch_series({});
};
