#include "mcp/client.h"

#include <memory>

#include "absl/status/status.h"

#include "mcp/session.h"
#include "mcp/streamable_http_transport.h"

namespace slop::mcp {

absl::StatusOr<std::unique_ptr<v2025_11_25::Session>> ConnectClassicStreamableHttp(
    const StreamableHttpConfig& config, const v2025_11_25::InitializeOptions& options, HttpClient* http_client) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  auto transport = std::make_unique<v2025_11_25::StreamableHttpTransport>(config, http_client);
  auto session = std::make_unique<v2025_11_25::Session>(std::move(transport));
  const absl::Status status = session->Initialize(options);
  if (!status.ok()) return status;
  return session;
}

}  // namespace slop::mcp
