#ifndef SLOP_MCP_SERVER_SERVER_H_
#define SLOP_MCP_SERVER_SERVER_H_

#include <optional>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

#include "mcp/types.h"

namespace slop::mcp::server {

// Transport-independent, latest-only MCP dispatcher. No tools are advertised
// until tool registration is implemented. Dispatch does not perform I/O.
class Server {
 public:
  static absl::StatusOr<Server> Create(ImplementationInfo identity);

  // Returns a JSON-RPC response, or no response for a valid notification.
  // Malformed messages return protocol errors rather than operational statuses.
  std::optional<nlohmann::json> Dispatch(absl::string_view raw) const;

 private:
  explicit Server(ImplementationInfo identity);
  ImplementationInfo identity_;
};

}  // namespace slop::mcp::server

#endif  // SLOP_MCP_SERVER_SERVER_H_
