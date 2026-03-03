return function(args) {
  const name = args.name;
  const code = args.code;
  const description = args.description || "";
  const test_args = args.test_args || [];
  const expected_result = args.expected_result;

  if (typeof name !== "string" || typeof code !== "string") {
    return [false, "Invalid arguments: name and code must be strings"];
  }

  let func;
  try {
    let eval_code = code;
    if (eval_code.trim().startsWith("return ")) {
      eval_code = "(function() { " + eval_code + " })()";
    }
    func = eval(eval_code);
  } catch (e) {
    return [false, "Syntax/Evaluation Error: " + e.message];
  }

  if (typeof func !== "function") {
    return [false, "Code must return a function"];
  }

  let actual_result;
  try {
    actual_result = func.apply(null, test_args);
  } catch (e) {
    return [false, "Runtime Error: " + e.message];
  }

  if (actual_result !== expected_result) {
    return [false, "Test Failed: Expected " + expected_result + ", got " + actual_result];
  }

  try {
    tools.query_db({
      sql: "INSERT OR REPLACE INTO js_functions (name, code, description) VALUES (?, ?, ?)",
      params: [name, code, description]
    });
  } catch (e) {
    return [false, "DB Error: " + e.message];
  }

  globalThis[name] = func;

  return [true, "Function persisted successfully"];
};
