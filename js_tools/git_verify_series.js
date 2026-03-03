return function(args) {
  slop_guard();
  git_assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before running this command.");

  const command = args.command;
  if (!command) throw new Error("command is required");
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = `git log --reverse --format=%H ${shell_escape(base_branch)}..HEAD`;
  const log_res = tools.execute_bash({command: log_cmd});
  
  const commits = log_res.stdout.trim().split(/\s+/).filter(h => h.length > 0);
  const current_branch = git_get_current_branch();
  const results = [];
  let all_passed = true;
  
  for (let i = 0; i < commits.length; i++) {
    const hash = commits[i];
    const co_res = __os_run(`git checkout ${hash}`);
    if (co_res.exit_code !== 0) {
      results.push({status: "failed", message: "Checkout failed", hash: hash});
      all_passed = false;
      break;
    }
    const test_res = __os_run(command);
    const status = (test_res.exit_code === 0) ? "passed" : "failed";
    results.push({status: status, hash: hash});
    if (test_res.exit_code !== 0) {
      all_passed = false;
      break;
    }
  }
  
  __os_run(`git checkout ${shell_escape(current_branch)}`);
  
  return JSON.stringify({
    all_passed: all_passed,
    report: results
  });
};
