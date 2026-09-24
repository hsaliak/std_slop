#ifndef SLOP_MCP_MODERN_HTTP_H_
#define SLOP_MCP_MODERN_HTTP_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "mcp/client/modern.h"
#include "mcp/protocol.h"
#include "mcp/types.h"

namespace slop::mcp::v2026_07_28 {

struct HttpExchangeOptions {
  std::string endpoint_url;
  absl::flat_hash_map<std::string, std::string> extra_headers;
  std::optional<std::string> bearer_token;
  absl::Duration deadline = absl::Seconds(60);
  size_t max_response_bytes = 4 * 1024 * 1024;
  size_t max_messages = 1024;
};

struct HttpExchangeResult {
  long http_status = 0;
  std::optional<JsonRpcResponse> response;
  std::vector<ServerNotification> notifications;
  std::optional<ProtocolFailure> failure;
};

class HttpExchange {
 public:
  HttpExchange(HttpExchangeOptions options, HttpClient* http_client);

  absl::StatusOr<HttpExchangeResult> Execute(const Request& request);
  void Cancel();

 private:
  absl::Status ValidateOptions() const;
  absl::StatusOr<HttpExchangeResult> DecodeResponse(const Request& request, const HttpResponse& response) const;

  HttpExchangeOptions options_;
  HttpClient* http_client_;
};

}  // namespace slop::mcp::v2026_07_28

#endif  // SLOP_MCP_MODERN_HTTP_H_
