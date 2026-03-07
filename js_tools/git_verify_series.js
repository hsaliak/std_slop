return function(args) {
  const base_branch = (args && args.base_branch) || "main";
  const build_command = (args && args.build_command) || "npm test";

  const res = tools.execute_bash({ 
    command: "git log " + base_branch + "..HEAD --format=%H" 
  });
  const commits = res.stdout.trim().split("\n").reverse();

  const results = [];
  for (const hash of commits) {
    if (!hash) continue;
    tools.execute_bash({ command: "git checkout " + hash });
    
    let ok = true;
    let error = "";
    try {
      tools.execute_bash({ command: build_command });
    } catch (e) {
      ok = false;
      error = e.message;
    }
    
    results.push({ hash, ok, error });
    if (!ok) break;
  }

  tools.execute_bash({ command: "git checkout -" });

  return {
    ok: results.every(r => r.ok),
    results: results
  };
};

