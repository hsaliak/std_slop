return function(args) {
  slop_guard();
  const index = parseInt(args.index, 10);
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.stdout.trim().split(/\s+/).filter(h => h.length > 0);
  
  if (isNaN(index) || index < 1 || index > commits.length) {
    throw new Error("Invalid patch index " + index + " (total patches: " + commits.length + ")");
  }
  
  const target_hash = commits[index - 1];
  
  const commit_res = __os_run(`git commit --fixup ${target_hash}`);
  if (commit_res.exit_code !== 0) throw new Error("Failed to create fixup commit. Are there any changes staged?");
  
  const env_cmd = `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash ${shell_escape(base_branch)}`;
  const rebase_res = __os_run(env_cmd);
  if (rebase_res.exit_code !== 0) {
    __os_run("git rebase --abort");
    throw new Error("Rebase failed. You may have conflicts. Manual intervention required.\n" + rebase_res.stderr);
  }
  
  return tools.git_format_patch_series({base_branch: base_branch});
};
