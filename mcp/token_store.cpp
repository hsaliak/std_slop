#include "mcp/token_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "core/json_utils.h"

namespace slop::mcp {
namespace {

bool ContainsHttpHeaderControlCharacter(const std::string& value) {
  for (const char c : value) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (ch < 0x20 || ch == 0x7f) return true;
  }
  return false;
}

absl::Status ValidateAccessTokenForHeader(const std::string& access_token) {
  if (access_token.empty()) return absl::InvalidArgumentError("OAuth access token must not be empty");
  if (ContainsHttpHeaderControlCharacter(access_token)) {
    return absl::InvalidArgumentError("OAuth access token must not contain HTTP header control characters");
  }
  return absl::OkStatus();
}

}  // namespace


absl::Status SaveOAuthTokens(const std::string& path, const OAuthTokenSet& tokens) {
  const absl::Status token_status = ValidateAccessTokenForHeader(tokens.access_token);
  if (!token_status.ok()) return token_status;
  const std::filesystem::path token_path(path);
  std::error_code error;
  if (!token_path.parent_path().empty()) {
    std::filesystem::create_directories(token_path.parent_path(), error);
    if (error) return absl::UnavailableError(absl::StrCat("Failed to create token directory: ", error.message()));
  }
  const std::string content = json_dump({{"access_token", tokens.access_token},
                                         {"refresh_token", tokens.refresh_token},
                                         {"expires_at", tokens.expires_at_unix_seconds}});
  std::string temporary_template = absl::StrCat(path, ".tmp.XXXXXX");
  std::vector<char> buffer(temporary_template.begin(), temporary_template.end());
  buffer.push_back('\0');
  const int fd = mkstemp(buffer.data());
  if (fd < 0) return absl::UnavailableError("Failed to create token file");
  (void)fchmod(fd, 0600);
  const ssize_t written = write(fd, content.data(), content.size());
  const int close_status = close(fd);
  if (written != static_cast<ssize_t>(content.size()) || close_status != 0) {
    std::filesystem::remove(buffer.data(), error);
    return absl::UnavailableError("Failed to write token file");
  }
  std::filesystem::rename(buffer.data(), token_path, error);
  if (error) {
    std::filesystem::remove(buffer.data(), error);
    return absl::UnavailableError(absl::StrCat("Failed to replace token file: ", error.message()));
  }
  return absl::OkStatus();
}

absl::StatusOr<OAuthTokenSet> LoadOAuthTokens(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return absl::NotFoundError("OAuth token file not found");
  const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  auto parsed = json_parse(content);
  if (!parsed || !parsed->is_object()) return absl::InvalidArgumentError("OAuth token file is invalid");
  OAuthTokenSet tokens;
  tokens.access_token = json_get_or(*parsed, "access_token", std::string{});
  tokens.refresh_token = json_get_or(*parsed, "refresh_token", std::string{});
  tokens.expires_at_unix_seconds = json_get_or(*parsed, "expires_at", int64_t{0});
  const absl::Status token_status = ValidateAccessTokenForHeader(tokens.access_token);
  if (!token_status.ok()) return token_status;
  return tokens;
}

absl::Status DeleteOAuthTokens(const std::string& path) {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error) return absl::UnavailableError(absl::StrCat("Failed to delete token file: ", error.message()));
  if (!removed) return absl::NotFoundError("OAuth token file not found");
  return absl::OkStatus();
}

}  // namespace slop::mcp
