#ifndef SLOP_MCP_MODERN_H_
#define SLOP_MCP_MODERN_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "mcp/protocol.h"
#include "mcp/types.h"

namespace slop::mcp::v2026_07_28 {

inline constexpr char kProtocolVersionMetadata[] = "io.modelcontextprotocol/protocolVersion";
inline constexpr char kClientCapabilitiesMetadata[] = "io.modelcontextprotocol/clientCapabilities";
inline constexpr char kClientInfoMetadata[] = "io.modelcontextprotocol/clientInfo";
inline constexpr char kServerInfoMetadata[] = "io.modelcontextprotocol/serverInfo";

struct RequestContext {
  ImplementationInfo client_info;
  nlohmann::json client_capabilities = nlohmann::json::object();
};

struct Request {
  JsonRpcId id;
  std::string method;
  nlohmann::json params = nlohmann::json::object();
  RequestContext context;
  std::optional<nlohmann::json> tool_schema;
};

struct EncodedRequest {
  nlohmann::json body;
  std::vector<std::string> headers;
};

struct Discovery {
  std::vector<std::string> supported_versions;
  nlohmann::json capabilities = nlohmann::json::object();
  std::optional<std::string> instructions;
  std::optional<ImplementationInfo> server_info;
};

enum class ResultType { kComplete, kInputRequired };

struct Result {
  ResultType type = ResultType::kComplete;
  nlohmann::json value = nlohmann::json::object();
  std::optional<nlohmann::json> request_state;
};

absl::StatusOr<EncodedRequest> EncodeRequest(const Request& request);
absl::StatusOr<Discovery> ParseDiscovery(const nlohmann::json& result);
absl::StatusOr<Result> ParseResult(const nlohmann::json& result);
absl::StatusOr<std::vector<Tool>> ParseToolsList(const nlohmann::json& result);
absl::Status ValidateToolArguments(const Tool& tool, const nlohmann::json& arguments);
absl::StatusOr<ToolCallResult> ParseToolCallResult(const nlohmann::json& result,
                                                   const nlohmann::json* output_schema = nullptr);

}  // namespace slop::mcp::v2026_07_28

#endif  // SLOP_MCP_MODERN_H_
