#ifndef SLOP_MCP_AUTHORIZATION_H_
#define SLOP_MCP_AUTHORIZATION_H_

#include <cstddef>
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
  std::vector<std::string> code_challenge_methods_supported;
  bool client_id_metadata_document_supported = false;
  bool authorization_response_iss_parameter_supported = false;
};

struct ClientIdMetadataDocument {
  std::string client_id;
  std::vector<std::string> redirect_uris;
  std::string token_endpoint_auth_method = "none";
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
absl::StatusOr<ClientIdMetadataDocument> ParseClientIdMetadataDocument(const nlohmann::json& metadata,
                                                                       absl::string_view document_url);
absl::Status ValidateAuthorizationBinding(const ProtectedResourceMetadata& resource_metadata,
                                          absl::string_view expected_resource,
                                          const AuthorizationServerMetadata& server_metadata,
                                          absl::string_view selected_authorization_server);
absl::StatusOr<std::vector<std::string>> MergeAuthorizationScopes(const std::vector<std::string>& previously_requested,
                                                                  const std::vector<std::string>& challenge_scopes,
                                                                  size_t max_scopes = 64);

}  // namespace slop::mcp

#endif  // SLOP_MCP_AUTHORIZATION_H_
