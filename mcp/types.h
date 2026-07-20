#ifndef SLOP_MCP_TYPES_H_
#define SLOP_MCP_TYPES_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

using JsonRpcId = std::variant<std::monostate, int64_t, std::string>;

struct JsonRpcError {
  int code = 0;
  std::string message;
  nlohmann::json data = nlohmann::json::object();
};

struct JsonRpcResponse {
  JsonRpcId id;
  std::optional<nlohmann::json> result;
  std::optional<JsonRpcError> error;
};

struct ImplementationInfo {
  std::string name;
  std::string version;
  std::optional<std::string> title;
};

struct ClientCapabilities {
  bool roots = false;
  bool roots_list_changed = false;
  bool sampling = false;
  bool elicitation = false;
  nlohmann::json experimental = nlohmann::json::object();
};

struct ServerCapabilities {
  bool tools = false;
  bool tools_list_changed = false;
  bool resources = false;
  bool resources_subscribe = false;
  bool resources_list_changed = false;
  bool prompts = false;
  bool prompts_list_changed = false;
  bool logging = false;
  nlohmann::json raw = nlohmann::json::object();
};

struct Tool {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
  nlohmann::json input_schema = nlohmann::json::object();
  nlohmann::json output_schema = nlohmann::json::object();
  nlohmann::json annotations = nlohmann::json::object();
};

struct ToolCallResult {
  std::vector<nlohmann::json> content;
  bool is_error = false;
  nlohmann::json structured_content = nlohmann::json::object();
};

struct StreamableHttpConfig {
  std::string endpoint_url;
  absl::flat_hash_map<std::string, std::string> extra_headers;
  std::optional<std::string> bearer_token;
  absl::Duration request_timeout = absl::Seconds(60);
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_TYPES_H_
