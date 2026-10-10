#ifndef SLOP_MCP_VALIDATION_H_
#define SLOP_MCP_VALIDATION_H_

#include <cstddef>
#include <initializer_list>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

// Checks an HTTP(S) URL's scheme, authority, host, port, and escaped bytes.
// When require_https is true, plain HTTP is rejected.
bool IsValidHttpUrl(absl::string_view url, std::size_t max_bytes, bool require_https = false);
bool ContainsControlCharacters(absl::string_view value);
bool IsValidAbsolutePath(absl::string_view path, std::size_t max_bytes);

bool IsValidEnvironmentVariableName(absl::string_view name, std::size_t max_bytes = 256);
bool IsValidMcpServerName(absl::string_view name);
bool IsSafeGatewayServerAlias(absl::string_view alias);

// Ensures value is an object and contains only fields in allowed_fields.
absl::Status ValidateJsonObjectFields(const nlohmann::json& value,
                                      std::initializer_list<absl::string_view> allowed_fields,
                                      absl::string_view object_name);

}  // namespace slop::mcp

#endif  // SLOP_MCP_VALIDATION_H_
