return function() {
  try {
    const res = tools.query_db({sql: "SELECT mode FROM settings WHERE id = 1"});
    if (res && res.includes('"standard"')) {
      return;
    }
  } catch (e) { print("Error parsing JSON from DB: " + e.message); }

  const branch = git_get_current_branch();
  if (!branch) return;

  if (!branch.startsWith("slop/staging/") && branch !== "HEAD") {
    throw new Error("Destructive operations are only allowed on 'slop/staging/*' branches. Current branch: " + branch);
  }
};
