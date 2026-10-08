#ifndef SLOP_MCP_DECISION_API_CLIENT_H_
#define SLOP_MCP_DECISION_API_CLIENT_H_

#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "mcp/decision_api/config.h"

namespace slop::mcp::decision_api {

class DecisionApiClient {
 public:
  DecisionApiClient(Config config, slop::HttpClient* http_client)
      : config_(std::move(config)), http_client_(http_client) {}

  absl::StatusOr<nlohmann::json> Decide(const nlohmann::json& arguments) const;
  absl::StatusOr<nlohmann::json> Models() const;

  static absl::StatusOr<nlohmann::json> BuildRequest(const nlohmann::json& arguments, const Config& config);
  static absl::Status ValidateResponse(const nlohmann::json& request, const nlohmann::json& response);

 private:
  Config config_;
  slop::HttpClient* http_client_;
};

}  // namespace slop::mcp::decision_api

#endif  // SLOP_MCP_DECISION_API_CLIENT_H_
