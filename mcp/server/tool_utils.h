#ifndef SLOP_MCP_SERVER_TOOL_UTILS_H_
#define SLOP_MCP_SERVER_TOOL_UTILS_H_

#include <string>
#include <utility>

#include "mcp/server/server.h"
#include "mcp/types.h"

namespace slop::mcp::server {

inline ToolRegistration MakeToolRegistration(std::string name, std::string description, nlohmann::json input_schema,
                                            nlohmann::json annotations, ToolHandler handler) {
  ToolRegistration registration;
  registration.definition.name = std::move(name);
  registration.definition.description = std::move(description);
  registration.definition.input_schema = std::move(input_schema);
  registration.definition.annotations = std::move(annotations);
  registration.handler = std::move(handler);
  return registration;
}

inline mcp::ToolCallResult MakeToolCallResult(std::string text, nlohmann::json structured_content,
                                              bool is_error = false) {
  mcp::ToolCallResult result;
  result.content.push_back({{"type", "text"}, {"text", std::move(text)}});
  result.structured_content = std::move(structured_content);
  result.is_error = is_error;
  return result;
}

}  // namespace slop::mcp::server

#endif  // SLOP_MCP_SERVER_TOOL_UTILS_H_
