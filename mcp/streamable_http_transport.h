#ifndef SLOP_MCP_STREAMABLE_HTTP_TRANSPORT_H_
#define SLOP_MCP_STREAMABLE_HTTP_TRANSPORT_H_

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "core/http_client.h"
#include "mcp/transport.h"
#include "mcp/types.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

class StreamableHttpTransport : public Transport {
 public:
  StreamableHttpTransport(StreamableHttpConfig config, HttpClient* http_client);

  absl::Status Start() override;
  absl::Status Send(const nlohmann::json& message) override;
  absl::StatusOr<nlohmann::json> Receive(absl::Duration timeout) override;
  absl::Status Close() override;

  const std::string& session_id() const { return session_id_; }
  void SetProtocolVersion(absl::string_view protocol_version) override { protocol_version_ = std::string(protocol_version); }

 private:
  std::vector<std::string> BuildHeaders() const;
  absl::Status EnqueueResponseMessages(const HttpResponse& response);

  StreamableHttpConfig config_;
  HttpClient* http_client_;
  std::string protocol_version_;
  std::string session_id_;
  std::vector<nlohmann::json> pending_messages_;
  bool started_ = false;
  bool closed_ = false;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_STREAMABLE_HTTP_TRANSPORT_H_
