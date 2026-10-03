#ifndef SLOP_MCP_CLIENT_MODERN_EXCHANGE_H_
#define SLOP_MCP_CLIENT_MODERN_EXCHANGE_H_

#include <cstddef>
#include <optional>
#include <vector>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "mcp/client/modern.h"
#include "mcp/protocol.h"

namespace slop::mcp::v2026_07_28 {

struct ModernExchangeResult {
  std::optional<JsonRpcResponse> response;
  std::vector<ServerNotification> notifications;
  std::optional<ProtocolFailure> failure;
};

class ModernExchange {
 public:
  virtual ~ModernExchange() = default;
  virtual absl::StatusOr<ModernExchangeResult> Execute(const Request& request) = 0;
  virtual void Cancel() {}
};

// Validates common JSON-RPC response IDs and server notifications and maps
// protocol errors into a transport-independent result.
absl::StatusOr<ModernExchangeResult> NormalizeModernMessages(const Request& request,
                                                             std::vector<nlohmann::json> messages, size_t max_messages,
                                                             std::optional<long> http_status = std::nullopt);

}  // namespace slop::mcp::v2026_07_28

#endif  // SLOP_MCP_CLIENT_MODERN_EXCHANGE_H_
