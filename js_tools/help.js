return function(args) {
  const builtinSchemas = {
    ask_user: {
      type: "object",
      properties: {
        prompt: { type: "string", description: "Prompt text shown to the user." }
      },
      required: []
    },
    dispatch_async: {
      type: "object",
      properties: {
        name: { type: "string", description: "Tool name" },
        args: { type: "object", description: "Tool arguments" }
      },
      required: ["name", "args"]
    },
    query_db: {
      type: "object",
      properties: {
        sql: { type: "string", description: "SQL query" },
        params: {
          type: "array",
          description: "Optional positional parameters",
          items: {}
        }
      },
      required: ["sql"]
    }
  };

  const builtinDescriptions = {
    ask_user: "Prompts the user for required clarification/input.",
    dispatch_async: "Dispatches a tool call asynchronously and returns a job handle.",
    query_db: "Queries the SQLite database and returns JSON rows."
  };

  const rowsRaw = tools.query_db({
    sql: "SELECT name, description, json_schema FROM js_functions " +
         "WHERE json_schema IS NOT NULL AND TRIM(json_schema) <> ''"
  });

  let rows = [];
  try {
    rows = JSON.parse(rowsRaw);
  } catch (_e) {
    rows = [];
  }

  const persistedByName = {};
  for (const row of rows) {
    if (!row || typeof row.name !== "string") continue;
    persistedByName[row.name] = row;
  }

  const catalog = [];
  for (const name in tools) {
    if (typeof tools[name] !== "function") continue;

    const persisted = persistedByName[name];
    if (persisted) {
      catalog.push({
        name,
        description: (typeof persisted.description === "string") ? persisted.description : "",
        json_schema: (typeof persisted.json_schema === "string") ? persisted.json_schema : "",
        source: "persisted"
      });
      continue;
    }

    catalog.push({
      name,
      description: builtinDescriptions[name] || "",
      json_schema: builtinSchemas[name] ? JSON.stringify(builtinSchemas[name]) : "",
      source: "builtin"
    });
  }

  catalog.sort((a, b) => {
    if (a.source !== b.source) return a.source < b.source ? -1 : 1;
    return a.name.localeCompare(b.name);
  });

  return JSON.stringify(catalog);
};

