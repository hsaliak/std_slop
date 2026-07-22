#include "mcp/client.h"

#include <memory>

#include "absl/status/status.h"
#include "mcp/session.h"
#include "mcp/streamable_http_transport.h"

namespace slop::mcp {

absl::StatusOr<std::unique_ptr<Session>> ConnectStreamableHttp(const StreamableHttpConfig& config,
                                                               const InitializeOptions& options,
                                                               HttpClient* http_client) {
  if (http_client == nullptr) return absl::InvalidArgumentError("http_client must not be null");
  auto transport = std::make_unique<StreamableHttpTransport>(config, http_client);
  auto session = std::make_unique<Session>(std::move(transport));
  const absl::Status status = session->Initialize(options);
  if (!status.ok()) return status;
  return session;
}

}  // namespace slop::mcp
