#ifndef SLOP_MCP_JSON_SCHEMA_H_
#define SLOP_MCP_JSON_SCHEMA_H_

#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

struct JsonSchemaLimits {
  size_t max_evaluations = 10000;
  size_t max_depth = 64;
};

// Validates an instance against the supported JSON Schema 2020-12 subset.
// References are restricted to local JSON Pointers. Network and filesystem
// resolution are intentionally not supported.
absl::Status ValidateJsonSchema(const nlohmann::json& schema, const nlohmann::json& instance,
                                JsonSchemaLimits limits = JsonSchemaLimits());

// Checks schema structure, dialect, local references, and resource limits
// without requiring a representative instance.
absl::Status CheckJsonSchema(const nlohmann::json& schema, JsonSchemaLimits limits = JsonSchemaLimits());

}  // namespace slop::mcp

#endif  // SLOP_MCP_JSON_SCHEMA_H_
