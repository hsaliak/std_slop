return function(args) {
  const name = args.name;
  const base_branch = git_resolve_base_branch(args.base_branch);
  if (!name) throw new Error("name is required");

  const normalized_name = String(name).replace(/^(slop\/staging\/)+/, "");
  if (!normalized_name) throw new Error("name must contain non-prefix characters");
  const staging_name = "slop/staging/" + normalized_name;

  const cmd = `git checkout -b ${shell_escape(staging_name)} ${shell_escape(base_branch)}`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Failed to create staging branch: " + res.stdout + res.stderr);
  }

  tools.query_db({
    sql: "INSERT OR REPLACE INTO staging_branches (branch_name, parent_branch) VALUES (?, ?)",
    params: [staging_name, base_branch]
  });
  
  return "Created and checked out staging branch: " + staging_name + " (base: " + base_branch + ")";
};

