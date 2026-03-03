return function(msg) {
  const status_res = __os_run("git status --porcelain");
  if (status_res.stdout !== "") {
    throw new Error(msg || "Working tree is dirty. Please commit, stash, or discard changes.");
  }
};
