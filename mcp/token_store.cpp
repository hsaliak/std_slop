#include "mcp/token_store.h"

#include "mcp/token_store_internal.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"

#include <sys/stat.h>

namespace slop::mcp {

absl::Status token_store_internal::WriteAll(
    int fd, absl::string_view content, WriteFunction write_function) {
  size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written =
        write_function(fd, content.data() + offset, content.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return absl::UnavailableError(
          absl::StrCat("Failed to write token file: ", std::strerror(errno)));
    }
    if (written == 0) {
      return absl::UnavailableError("Failed to write token file: no progress");
    }
    offset += static_cast<size_t>(written);
  }
  return absl::OkStatus();
}

namespace {

constexpr size_t kMaxTokenFileBytes = 4 * 1024 * 1024;

absl::Status ErrnoStatus(absl::string_view operation) {
  return absl::UnavailableError(absl::StrCat(operation, ": ", std::strerror(errno)));
}

absl::StatusOr<std::string> ReadAll(int fd) {
  std::string content;
  char buffer[8192];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("Failed to read OAuth token file");
    }
    if (count == 0) return content;
    if (static_cast<size_t>(count) > kMaxTokenFileBytes - content.size()) {
      return absl::ResourceExhaustedError("OAuth token file exceeds size limit");
    }
    content.append(buffer, static_cast<size_t>(count));
  }
}

absl::Status SyncParentDirectory(const std::filesystem::path& path) {
  const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
  const int directory_fd = open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory_fd < 0) return ErrnoStatus("Failed to open token directory");
  if (fsync(directory_fd) != 0) {
    const absl::Status status = ErrnoStatus("Failed to sync token directory");
    close(directory_fd);
    return status;
  }
  if (close(directory_fd) != 0) return ErrnoStatus("Failed to close token directory");
  return absl::OkStatus();
}

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
  if (!absl::EqualsIgnoreCase(tokens.token_type, "Bearer")) {
    return absl::InvalidArgumentError("OAuth token file token_type must be Bearer");
  }
  const std::filesystem::path token_path(path);
  std::error_code error;
  if (!token_path.parent_path().empty()) {
    std::filesystem::create_directories(token_path.parent_path(), error);
    if (error) return absl::UnavailableError(absl::StrCat("Failed to create token directory: ", error.message()));
  }
  const std::string content = json_dump({
      {"access_token", tokens.access_token},
      {"refresh_token", tokens.refresh_token},
      {"token_type", "Bearer"},
      {"scope", tokens.scope},
      {"issuer", tokens.issuer},
      {"resource", tokens.resource},
      {"expires_at", tokens.expires_at_unix_seconds},
  });
  std::string temporary_template = absl::StrCat(path, ".tmp.XXXXXX");
  std::vector<char> buffer(temporary_template.begin(), temporary_template.end());
  buffer.push_back('\0');
  const int fd = mkstemp(buffer.data());
  if (fd < 0) return absl::UnavailableError("Failed to create token file");
  if (fchmod(fd, 0600) != 0) {
    close(fd);
    std::filesystem::remove(buffer.data(), error);
    return absl::PermissionDeniedError("Failed to restrict temporary token file permissions");
  }
  const absl::Status write_status = token_store_internal::WriteAll(
      fd, content, [](int output_fd, const void* data, size_t size) {
        return write(output_fd, data, size);
      });
  const absl::Status sync_status =
      write_status.ok() && fsync(fd) != 0 ? ErrnoStatus("Failed to sync token file") : absl::OkStatus();
  const absl::Status close_status = close(fd) != 0 ? ErrnoStatus("Failed to close token file") : absl::OkStatus();
  if (!write_status.ok() || !sync_status.ok() || !close_status.ok()) {
    std::filesystem::remove(buffer.data(), error);
    if (!write_status.ok()) return write_status;
    if (!sync_status.ok()) return sync_status;
    return close_status;
  }
  std::filesystem::rename(buffer.data(), token_path, error);
  if (error) {
    std::filesystem::remove(buffer.data(), error);
    return absl::UnavailableError(absl::StrCat("Failed to replace token file: ", error.message()));
  }
  return SyncParentDirectory(token_path);
}

absl::StatusOr<OAuthTokenSet> LoadOAuthTokens(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) return absl::NotFoundError("OAuth token file not found");
    if (errno == ELOOP) return absl::PermissionDeniedError("OAuth token file must not be a symlink");
    return ErrnoStatus("OAuth token file could not be opened");
  }
  struct stat file_stat;
  if (fstat(fd, &file_stat) != 0) {
    const absl::Status status = ErrnoStatus("OAuth token file could not be inspected");
    close(fd);
    return status;
  }
  if (!S_ISREG(file_stat.st_mode) || (file_stat.st_mode & 0777) != 0600) {
    close(fd);
    return absl::PermissionDeniedError("OAuth token file must be a regular 0600 file");
  }
  auto content = ReadAll(fd);
  const absl::Status close_status = close(fd) != 0 ? ErrnoStatus("OAuth token file could not be closed")
                                                   : absl::OkStatus();
  if (!content.ok()) return content.status();
  if (!close_status.ok()) return close_status;
  auto parsed = json_parse(*content);
  if (!parsed || !parsed->is_object()) return absl::InvalidArgumentError("OAuth token file is invalid");
  OAuthTokenSet tokens;
  tokens.access_token = json_get_or(*parsed, "access_token", std::string{});
  tokens.refresh_token = json_get_or(*parsed, "refresh_token", std::string{});
  const auto token_type = json_get<std::string>(*parsed, "token_type");
  if (!token_type.has_value()) {
    return absl::InvalidArgumentError("OAuth token file missing token_type");
  }
  tokens.token_type = *token_type;
  tokens.scope = json_get_or(*parsed, "scope", std::string{});
  tokens.issuer = json_get_or(*parsed, "issuer", std::string{});
  tokens.resource = json_get_or(*parsed, "resource", std::string{});
  tokens.expires_at_unix_seconds = json_get_or(*parsed, "expires_at", int64_t{0});
  if (!absl::EqualsIgnoreCase(tokens.token_type, "Bearer")) {
    return absl::InvalidArgumentError("OAuth token file token_type must be Bearer");
  }
  tokens.token_type = "Bearer";
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
