return function(args) {
  slop_guard();
  git_assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before finalizing.");

  const current_branch = git_get_current_branch();
  const target_branch = git_resolve_base_branch(args.target_branch);

  const hash_res = tools.execute_bash({command: "git rev-parse HEAD"});
  const hash = hash_res.stdout.trim();

  const approval_res = tools.query_db({
    sql: "SELECT approved_hash FROM patch_approvals WHERE branch_name = ?",
    params: [current_branch]
  });
  
  let approved = false;
  if (approval_res) {
    try {
      const rows = JSON.parse(approval_res);
      for (const row of rows) {
        if (row.approved_hash === hash) {
          approved = true;
          break;
        }
      }
    } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }
  
  if (!approved) {
    throw new Error("Patch series not approved or hash mismatch. Please obtain approval for hash " + hash + " before finalizing.");
  }

  const res1 = __os_run(`git checkout ${shell_escape(target_branch)}`);
  if (res1.exit_code !== 0) {
    throw new Error("Failed to checkout target branch '" + target_branch + "': " + res1.stderr);
  }

  const res2 = __os_run(`git merge --ff-only ${shell_escape(current_branch)}`);
  if (res2.exit_code !== 0) {
    __os_run(`git checkout ${shell_escape(current_branch)}`);
    throw new Error("Merge failed: " + res2.stderr);
  }

  __os_run(`git branch -D ${shell_escape(current_branch)}`);
  
  tools.query_db({
    sql: "DELETE FROM staging_branches WHERE branch_name = ?",
    params: [current_branch]
  });
  tools.query_db({
    sql: "DELETE FROM patch_approvals WHERE branch_name = ?",
    params: [current_branch]
  });

  return "Successfully finalized series and merged into " + target_branch;
};
