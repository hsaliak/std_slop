return function(args) {
  slop_guard();
  const base_branch = git_resolve_base_branch(args.base_branch);
  
  const log_cmd = "git log --reverse --format='### Patch [%n/%N] ###%ncommit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n%n%b' " + shell_escape(base_branch) + "..HEAD";
  const log_res = tools.execute_bash({command: log_cmd});
  
  const diff_cmd = `git diff ${shell_escape(base_branch)}..HEAD`;
  const diff_res = tools.execute_bash({command: diff_cmd});
  
  return "--- MAIL SERIES ---\nBase: " + base_branch + "\n\n" + log_res.output + "\n\n--- FULL DIFF ---\n" + diff_res.output;
};
