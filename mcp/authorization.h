#ifndef SLOP_MCP_AUTHORIZATION_H_
#define SLOP_MCP_AUTHORIZATION_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

struct ProtectedResourceMetadata {
  std::string resource;
  std::vector<std::string> authorization_servers;
  std::vector<std::string> scopes_supported;
};

struct AuthorizationServerMetadata {
  std::string issuer;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::vector<std::string> scopes_supported;
};

class TokenProvider {
 public:
  virtual ~TokenProvider() = default;
  virtual absl::StatusOr<std::string> GetAccessToken(absl::string_view server_name) = 0;
  virtual absl::Status Refresh(absl::string_view server_name) = 0;
};

absl::StatusOr<std::string> ParseWwwAuthenticateResourceMetadata(absl::string_view header);
absl::StatusOr<ProtectedResourceMetadata> ParseProtectedResourceMetadata(const nlohmann::json& metadata);
absl::StatusOr<AuthorizationServerMetadata> ParseAuthorizationServerMetadata(const nlohmann::json& metadata);

}  // namespace slop::mcp

#endif  // SLOP_MCP_AUTHORIZATION_H_
