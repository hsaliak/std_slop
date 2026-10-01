#ifndef SLOP_MCP_SERVER_SERVER_H_
#define SLOP_MCP_SERVER_SERVER_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

#include "mcp/types.h"

namespace slop::mcp::server {

using ToolHandler = std::function<absl::StatusOr<ToolCallResult>(const nlohmann::json&)>;

struct ToolRegistration {
  // Empty output_schema means no declared output schema, matching Tool defaults.
  Tool definition;
  ToolHandler handler;
};

// Transport-independent, latest-only MCP dispatcher. Registrations are validated
// and frozen at creation. Dispatch does not perform I/O; handlers own their side
// effects and must not write to a future stdio transport's protocol stream.
class Server {
 public:
  // A non-OK handler status is a server error. Actionable tool errors should
  // be returned as ToolCallResult with is_error set. Only complete results are
  // supported; resumable calls and pagination are not implemented.
  static absl::StatusOr<Server> Create(ImplementationInfo identity, std::vector<ToolRegistration> tools = {});

  // Returns a JSON-RPC response, or no response for a valid notification.
  // Malformed messages return protocol errors rather than operational statuses.
  std::optional<nlohmann::json> Dispatch(absl::string_view raw) const;

 private:
  Server(ImplementationInfo identity, std::map<std::string, ToolRegistration> tools);
  ImplementationInfo identity_;
  std::map<std::string, ToolRegistration> tools_;
};

}  // namespace slop::mcp::server

#endif  // SLOP_MCP_SERVER_SERVER_H_
