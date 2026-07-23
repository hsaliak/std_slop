#ifndef SLOP_MCP_TOKEN_STORE_H_
#define SLOP_MCP_TOKEN_STORE_H_

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace slop::mcp {

struct OAuthTokenSet {
  std::string access_token;
  std::string refresh_token;
  int64_t expires_at_unix_seconds = 0;
};

absl::Status SaveOAuthTokens(const std::string& path, const OAuthTokenSet& tokens);
absl::StatusOr<OAuthTokenSet> LoadOAuthTokens(const std::string& path);
absl::Status DeleteOAuthTokens(const std::string& path);

}  // namespace slop::mcp

#endif  // SLOP_MCP_TOKEN_STORE_H_
