#include "mcp/client/http_headers.h"

#include <string>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace slop::mcp {
namespace {

bool IsHttpToken(absl::string_view value) {
  if (value.empty()) return false;
  for (const unsigned char c : value) {
    if (absl::ascii_isalnum(c)) continue;
    switch (c) {
      case '!':
      case '#':
      case '$':
      case '%':
      case '&':
      case '\'':
      case '*':
      case '+':
      case '-':
      case '.':
      case '^':
      case '_':
      case '`':
      case '|':
      case '~':
        continue;
      default:
        return false;
    }
  }
  return true;
}

bool IsReservedHeader(absl::string_view name) {
  const std::string lower = absl::AsciiStrToLower(name);
  return lower == "content-type" || lower == "accept" || lower == "authorization" || lower == "mcp-protocol-version" ||
         lower == "mcp-method" || lower == "mcp-name" || lower == "mcp-session-id" || lower == "last-event-id" ||
         absl::StartsWith(lower, "mcp-param-");
}

}  // namespace

absl::Status ValidateExtraHeaders(const absl::flat_hash_map<std::string, std::string>& headers) {
  for (const auto& [name, value] : headers) {
    if (!IsHttpToken(name) || IsReservedHeader(name)) {
      return absl::InvalidArgumentError(absl::StrCat("reserved or invalid MCP header: ", name));
    }
    for (const unsigned char c : value) {
      if (c < 0x20 || c == 0x7f) {
        return absl::InvalidArgumentError("invalid MCP extra header value");
      }
    }
  }
  return absl::OkStatus();
}

}  // namespace slop::mcp
