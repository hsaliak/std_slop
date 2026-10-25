#ifndef SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_
#define SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_

#include <string>
#include <utility>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "mcp/server/server.h"
#include "mcp/types.h"

namespace slop::mcp::local_tools {

inline mcp::ToolCallResult MakeToolResult(std::string text, nlohmann::json structured_content,
                                          bool is_error = false) {
  mcp::ToolCallResult result;
  result.content.push_back({{"type", "text"}, {"text", std::move(text)}});
  result.structured_content = std::move(structured_content);
  result.is_error = is_error;
  return result;
}

inline mcp::ToolCallResult MakeJsonToolResult(nlohmann::json structured_content, bool is_error = false) {
  std::string text = json_dump(structured_content);
  return MakeToolResult(std::move(text), std::move(structured_content), is_error);
}

inline mcp::ToolCallResult MakeToolError(const absl::Status& status) {
  return MakeJsonToolResult({{"error", {{"message", std::string(status.message())}}}}, true);
}

inline mcp::server::ToolRegistration MakeToolRegistration(std::string name, std::string description,
                                                          nlohmann::json input_schema, nlohmann::json annotations,
                                                          mcp::server::ToolHandler handler) {
  mcp::server::ToolRegistration registration;
  registration.definition.name = std::move(name);
  registration.definition.description = std::move(description);
  registration.definition.input_schema = std::move(input_schema);
  registration.definition.annotations = std::move(annotations);
  registration.handler = std::move(handler);
  return registration;
}

}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_TOOL_UTILS_H_
