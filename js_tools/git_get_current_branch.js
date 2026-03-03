return function() {
  const forced_res = __os_run("printf %s \"$SLOP_FORCE_BRANCH_NAME\"");
  if (forced_res.exit_code === 0 && forced_res.stdout !== "") return forced_res.stdout;
  
  try {
    const res = __os_run("git rev-parse --abbrev-ref HEAD 2>/dev/null");
    if (res.exit_code === 0) {
      return res.stdout.trim();
    }
    return null;
  } catch (e) {
    return null;
  }
};
