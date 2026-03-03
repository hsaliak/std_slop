return function(args) {
  const name = args && args.name;
  const code = args && args.code;
  const description = (args && typeof args.description === "string") ? args.description : "";
  const test_args = (args && Array.isArray(args.test_args)) ? args.test_args : [];
  const has_expected_result = !!(args && Object.prototype.hasOwnProperty.call(args, "expected_result"));
  const expected_result = has_expected_result ? args.expected_result : undefined;

  const default_schema_obj = {
    type: "object",
    properties: {},
    required: []
  };

  let json_schema_str = JSON.stringify(default_schema_obj);
  if (args && Object.prototype.hasOwnProperty.call(args, "json_schema")) {
    if (typeof args.json_schema === "string") {
      try {
        const parsed = JSON.parse(args.json_schema);
        json_schema_str = JSON.stringify(parsed);
      } catch (e) {
        return [false, "Invalid arguments: json_schema must be valid JSON if provided as string"];
      }
    } else if (args.json_schema && typeof args.json_schema === "object") {
      try {
        json_schema_str = JSON.stringify(args.json_schema);
      } catch (e) {
        return [false, "Invalid arguments: json_schema object is not serializable"];
      }
    } else {
      return [false, "Invalid arguments: json_schema must be an object or JSON string"];
    }
  }

  if (typeof name !== "string" || typeof code !== "string") {
    return [false, "Invalid arguments: name and code must be strings"];
  }

  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    return [false, "Invalid arguments: name must match ^[A-Za-z_][A-Za-z0-9_]*$"];
  }

  const reservedNames = {
    tools: true,
    core: true,
    state: true,
    history: true,
    session_id: true,
    globalThis: true,
    __proto__: true,
    prototype: true,
    constructor: true
  };
  if (reservedNames[name]) {
    return [false, "Invalid arguments: name is reserved"];
  }

  if (typeof tools[name] === "function" || typeof globalThis[name] === "function") {
    return [false, "Invalid arguments: name conflicts with an existing tool or global"];
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

  function comparable(value) {
    if (value === null) return "null";
    const t = typeof value;
    if (t === "number" || t === "string" || t === "boolean" || t === "undefined") {
      return String(value);
    }
    try {
      return JSON.stringify(value);
    } catch (_e) {
      return "[unserializable]";
    }
  }

  if (has_expected_result) {
    let actual_result;
    try {
      actual_result = func.apply(null, test_args);
    } catch (e) {
      return [false, "Runtime Error: " + e.message];
    }

    let matches = (actual_result === expected_result);
    if (!matches) {
      const actualType = typeof actual_result;
      const expectedType = typeof expected_result;
      const canJsonCompare = (actual_result && (actualType === "object")) ||
                             (expected_result && (expectedType === "object"));
      if (canJsonCompare) {
        try {
          matches = JSON.stringify(actual_result) === JSON.stringify(expected_result);
        } catch (_e) {
          matches = false;
        }
      }
    }

    if (!matches) {
      return [false, "Test Failed: Expected " + comparable(expected_result) + ", got " + comparable(actual_result)];
    }
  }

  try {
    tools.query_db({
      sql: "INSERT OR REPLACE INTO js_functions (name, code, description, json_schema) VALUES (?, ?, ?, ?)",
      params: [name, code, description, json_schema_str]
    });
  } catch (e) {
    return [false, "DB Error: " + e.message];
  }

  tools[name] = func;
  globalThis[name] = func;

  return [true, "Function persisted successfully"];
};

