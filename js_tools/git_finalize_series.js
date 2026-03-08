return function(args) {
  slop_guard();
  git_assert_clean_workspace("Working tree is dirty. Please commit, stash, or discard changes before finalizing.");

  const canonical_staging = function(branch) {
    if (!branch) return branch;
    const m = String(branch).match(/^(?:slop\/staging\/)+(.*)$/);
    if (!m) return String(branch);
    return "slop/staging/" + m[1];
  };

  const current_branch = git_get_current_branch();
  const target_branch = git_resolve_base_branch(args.target_branch);

  const hash_res = tools.execute_bash({command: "git rev-parse HEAD"});
  const hash = hash_res.stdout.trim();

  const approval_res = tools.query_db({
    sql: "SELECT branch_name, approved_hash FROM patch_approvals WHERE approved_hash = ?",
    params: [hash]
  });

  let approved = false;
  const canonical_current = canonical_staging(current_branch);
  if (approval_res) {
    try {
      const rows = JSON.parse(approval_res);
      for (const row of rows) {
        if (row.approved_hash !== hash) continue;
        if (row.branch_name === current_branch || canonical_staging(row.branch_name) === canonical_current) {
          approved = true;
          break;
        }
      }
    } catch (e) { print("Error parsing JSON from DB: " + e.message); }
  }

  const landed_res = __os_run(`git merge-base --is-ancestor ${shell_escape(hash)} ${shell_escape(target_branch)}`);
  const already_landed = landed_res.exit_code === 0;

  if (!approved && !already_landed) {
    throw new Error("Patch series not approved or hash mismatch. Please obtain approval for hash " + hash + " before finalizing.");
  }

  const res1 = __os_run(`git checkout ${shell_escape(target_branch)}`);
  if (res1.exit_code !== 0) {
    throw new Error("Failed to checkout target branch '" + target_branch + "': " + res1.stderr);
  }

  if (!already_landed) {
    const res2 = __os_run(`git merge --ff-only ${shell_escape(current_branch)}`);
    if (res2.exit_code !== 0) {
      __os_run(`git checkout ${shell_escape(current_branch)}`);
      throw new Error("Merge failed: " + res2.stderr);
    }
  }

  let deleted_staging_branch = false;
  if (current_branch !== target_branch) {
    const del_res = __os_run(`git branch -D ${shell_escape(current_branch)}`);
    deleted_staging_branch = del_res.exit_code === 0;
  }

  tools.query_db({
    sql: "DELETE FROM staging_branches WHERE branch_name = ?",
    params: [current_branch]
  });
  tools.query_db({
    sql: "DELETE FROM staging_branches WHERE branch_name = ?",
    params: [canonical_current]
  });
  tools.query_db({
    sql: "DELETE FROM patch_approvals WHERE branch_name = ?",
    params: [current_branch]
  });
  tools.query_db({
    sql: "DELETE FROM patch_approvals WHERE branch_name = ?",
    params: [canonical_current]
  });

  // Explicitly exit mail mode after a successful finalize.
  // This avoids post-finalize policy confusion when the branch is now main.
  tools.query_db({
    sql: "UPDATE settings SET mode = 'standard' WHERE id = 1"
  });

  const head_res = tools.execute_bash({command: "git rev-parse HEAD"});
  const final_head = (head_res && head_res.stdout) ? head_res.stdout.trim() : "";

  return {
    ok: true,
    action: "finalize_series",
    mail_mode: "off",
    previous_branch: current_branch,
    current_branch: target_branch,
    head: final_head,
    approved: approved,
    already_landed: already_landed,
    merged: !already_landed,
    deleted_staging_branch: deleted_staging_branch,
    cleaned_metadata: true,
    notes: already_landed
      ? ["Patch already landed on target branch", "Cleaned staging metadata", "Mail mode disabled"]
      : ["Series finalized and merged", "Cleaned staging metadata", "Mail mode disabled"]
  };
};

