return function parseToolRows(value, context) {
  if (Array.isArray(value)) return value;
  if (value == null || value === "") return [];

  if (typeof value === "string") {
    let parsed;
    try {
      parsed = JSON.parse(value);
    } catch (e) {
      throw new Error("Failed to parse " + context + ": " + e.message);
    }
    if (Array.isArray(parsed)) return parsed;
    throw new Error("Unexpected result shape for " + context);
  }

  if (typeof value === "object" && Array.isArray(value.rows)) return value.rows;
  throw new Error("Unexpected result shape for " + context);
};

