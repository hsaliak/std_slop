return function(args) {
  const name = args.name;
  const base_branch = args.base_branch || git_get_current_branch();
  const staging_name = "slop/staging/" + name;
  if (!name) throw new Error("name is required");

  let res = __os_run(`git checkout -b ${shell_escape(staging_name)} ${shell_escape(base_branch)}`);
  if (res.exit_code !== 0 && (res.stdout + res.stderr).includes('already exists')) {
    res = __os_run(`git checkout ${shell_escape(staging_name)}`);
  }
  if (res.exit_code !== 0) {
    throw new Error("Failed to create staging branch: " + res.stdout + res.stderr);
  }

  tools.query_db({
    sql: "INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
    params: [staging_name, base_branch]
  });

  return "Created and checked out staging branch: " + staging_name + " (base: " + base_branch + ")";
};
