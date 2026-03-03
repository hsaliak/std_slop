return function() {
  const branch = git_get_current_branch();
  return branch && branch.startsWith("slop/staging/");
};
