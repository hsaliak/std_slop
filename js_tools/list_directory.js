return function(args) {
  const path = args.path || ".";
  const depth = args.depth || 1;
  
  const cmd = `find ${shell_escape(path)} -maxdepth ${depth} -mindepth 1`;
  const res = __os_run(cmd);
  if (res.exit_code !== 0) {
    throw new Error("Failed to list directory: " + res.stderr);
  }
  
  const lines = res.stdout.split("\n").filter(l => l.trim() !== "");
  const output = [];
  
  for (const line of lines) {
    let rel = line;
    if (line.startsWith(path)) {
      rel = line.substring(path.length);
      if (rel.startsWith("/")) rel = rel.substring(1);
    }
    
    if (rel !== "") {
      const type_check_cmd = `if [ -d ${shell_escape(line)} ]; then echo Directory; else echo File; fi`;
      const type_res = __os_run(type_check_cmd);
      if (type_res.stdout.includes("Directory")) {
        output.push(`Directory: ${rel}/`);
      } else {
        output.push(`File: ${rel}`);
      }
    }
  }
  
  return output.join("\n");
};
