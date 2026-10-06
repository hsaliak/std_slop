#ifndef SLOP_MCP_GATEWAY_RESULT_H_
#define SLOP_MCP_GATEWAY_RESULT_H_

#include <cstddef>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "mcp/types.h"

namespace slop::mcp::gateway {

struct NormalizedToolResult {
  bool ok;
  nlohmann::json value;
  std::string error_category;
  std::string error;
};

absl::StatusOr<NormalizedToolResult> NormalizeToolResult(const ToolCallResult& result,
                                                         std::size_t max_bytes = 1024 * 1024);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_RESULT_H_
