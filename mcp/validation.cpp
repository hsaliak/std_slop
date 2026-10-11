#include "mcp/validation.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"

namespace slop::mcp {
namespace {

bool IsValidPort(absl::string_view port) {
  if (port.empty()) return false;
  unsigned int value = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  return parsed.ec == std::errc() && parsed.ptr == port.data() + port.size() && value > 0 && value <= 65535;
}

bool IsAsciiAlphaNumeric(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool IsValidDnsName(absl::string_view host) {
  if (host.empty() || host.size() > 254) return false;
  host = absl::StripSuffix(host, ".");
  if (host.empty() || host.size() > 253) return false;

  while (!host.empty()) {
    const std::size_t separator = host.find('.');
    const absl::string_view label = host.substr(0, separator);
    if (label.empty() || label.size() > 63 || !IsAsciiAlphaNumeric(label.front()) ||
        !IsAsciiAlphaNumeric(label.back())) {
      return false;
    }
    for (const unsigned char c : label) {
      if (!IsAsciiAlphaNumeric(c) && c != '-') return false;
    }
    if (separator == absl::string_view::npos) break;
    host.remove_prefix(separator + 1);
  }
  return true;
}

bool IsUnreserved(unsigned char c) { return IsAsciiAlphaNumeric(c) || c == '-' || c == '.' || c == '_' || c == '~'; }

bool IsSubDelimiter(unsigned char c) {
  constexpr absl::string_view kSubDelimiters = "!$&'()*+,;=";
  return kSubDelimiters.find(static_cast<char>(c)) != absl::string_view::npos;
}

bool IsValidUriTail(absl::string_view tail) {
  bool in_query = false;
  for (std::size_t i = 0; i < tail.size(); ++i) {
    const unsigned char c = tail[i];
    if (c == '?' && !in_query) {
      in_query = true;
      continue;
    }
    if (c == '%') {
      if (i + 2 >= tail.size()) return false;
      const auto is_hex = [](unsigned char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
      };
      if (!is_hex(tail[i + 1]) || !is_hex(tail[i + 2])) return false;
      i += 2;
      continue;
    }
    const bool is_path_character = IsUnreserved(c) || IsSubDelimiter(c) || c == ':' || c == '@' || c == '/';
    if (!is_path_character && !(in_query && c == '?')) return false;
  }
  return true;
}

bool IsValidIpv4Address(absl::string_view address) {
  std::size_t octet_count = 0;
  while (!address.empty()) {
    const std::size_t separator = address.find('.');
    const absl::string_view octet = address.substr(0, separator);
    unsigned int value = 0;
    if (octet.empty() || (octet.size() > 1 && octet.front() == '0')) return false;
    const auto parsed = std::from_chars(octet.data(), octet.data() + octet.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != octet.data() + octet.size() || value > 255) return false;
    ++octet_count;
    if (separator == absl::string_view::npos) break;
    if (separator + 1 == address.size()) return false;
    address.remove_prefix(separator + 1);
  }
  return octet_count == 4;
}

bool IsValidIpv6Part(absl::string_view part, std::size_t* group_count, bool allow_ipv4_tail) {
  while (!part.empty()) {
    const std::size_t separator = part.find(':');
    const absl::string_view group = part.substr(0, separator);
    if (group.find('.') != absl::string_view::npos) {
      if (!allow_ipv4_tail || separator != absl::string_view::npos || !IsValidIpv4Address(group)) return false;
      *group_count += 2;
      return true;
    }
    if (group.empty() || group.size() > 4) return false;
    for (const unsigned char c : group) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    ++*group_count;
    if (separator == absl::string_view::npos) break;
    if (separator + 1 == part.size()) return false;
    part.remove_prefix(separator + 1);
  }
  return true;
}

bool IsValidIpv6Address(absl::string_view address) {
  const std::size_t compression = address.find("::");
  if (address.find(":::") != absl::string_view::npos) return false;
  std::size_t groups = 0;
  if (compression == absl::string_view::npos) {
    return IsValidIpv6Part(address, &groups, true) && groups == 8;
  }
  if (address.find("::", compression + 2) != absl::string_view::npos) return false;
  const absl::string_view prefix = address.substr(0, compression);
  const absl::string_view suffix = address.substr(compression + 2);
  return IsValidIpv6Part(prefix, &groups, false) && IsValidIpv6Part(suffix, &groups, true) && groups < 8;
}

bool IsValidAuthority(absl::string_view authority) {
  if (authority.empty() || authority.find('@') != absl::string_view::npos) return false;
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == absl::string_view::npos || close == 1) return false;
    const absl::string_view address = authority.substr(1, close - 1);
    if (!IsValidIpv6Address(address)) return false;
    const absl::string_view suffix = authority.substr(close + 1);
    return suffix.empty() || (suffix.front() == ':' && IsValidPort(suffix.substr(1)));
  }
  if (authority.find('[') != absl::string_view::npos || authority.find(']') != absl::string_view::npos) return false;
  const std::size_t colon = authority.find(':');
  const absl::string_view host = authority.substr(0, colon);
  const bool numeric_dotted_host =
      host.find('.') != absl::string_view::npos &&
      std::all_of(host.begin(), host.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; });
  if ((numeric_dotted_host && !IsValidIpv4Address(host)) || !IsValidDnsName(host) ||
      (colon != absl::string_view::npos && authority.find(':', colon + 1) != absl::string_view::npos)) {
    return false;
  }
  return colon == absl::string_view::npos || IsValidPort(authority.substr(colon + 1));
}

}  // namespace

bool ContainsControlCharacters(absl::string_view value) {
  for (const unsigned char character : value) {
    if (character < 0x20 || character == 0x7f) return true;
  }
  return false;
}

bool IsValidAbsolutePath(absl::string_view path, std::size_t max_bytes) {
  if (path.empty() || path.size() > max_bytes || ContainsControlCharacters(path)) return false;
  return std::filesystem::path(std::string(path.data(), path.size())).is_absolute();
}

bool IsValidHttpUrl(absl::string_view url, std::size_t max_bytes, bool require_https) {
  if (url.empty() || url.size() > max_bytes || ContainsControlCharacters(url) ||
      url.find('#') != absl::string_view::npos) {
    return false;
  }
  std::size_t authority_start = 0;
  if (absl::StartsWith(url, "https://")) {
    authority_start = 8;
  } else if (absl::StartsWith(url, "http://") && !require_https) {
    authority_start = 7;
  } else {
    return false;
  }
  const std::size_t authority_end = url.find_first_of("/?", authority_start);
  const absl::string_view authority =
      url.substr(authority_start,
                 authority_end == absl::string_view::npos ? absl::string_view::npos : authority_end - authority_start);
  if (!IsValidAuthority(authority)) return false;
  const absl::string_view uri_tail =
      authority_end == absl::string_view::npos ? absl::string_view() : url.substr(authority_end);
  return IsValidUriTail(uri_tail);
}

bool IsValidEnvironmentVariableName(absl::string_view name, std::size_t max_bytes) {
  if (name.empty() || name.size() > max_bytes) return false;
  const auto is_alpha_or_underscore = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
  };
  const auto is_digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  if (!is_alpha_or_underscore(static_cast<unsigned char>(name.front()))) return false;
  for (const unsigned char c : name) {
    if (!is_alpha_or_underscore(c) && !is_digit(c)) return false;
  }
  return true;
}

bool IsValidMcpServerName(absl::string_view name) {
  if (name.empty()) return false;
  for (const char c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool IsSafeGatewayServerAlias(absl::string_view alias) {
  if (alias.empty() || alias.size() > 64 ||
      !((alias[0] >= 'A' && alias[0] <= 'Z') || (alias[0] >= 'a' && alias[0] <= 'z') || alias[0] == '_')) {
    return false;
  }
  for (const char c : alias) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) {
      return false;
    }
  }
  static const std::unordered_set<absl::string_view> kReserved = {"await",
                                                                  "break",
                                                                  "case",
                                                                  "catch",
                                                                  "class",
                                                                  "const",
                                                                  "continue",
                                                                  "debugger",
                                                                  "default",
                                                                  "delete",
                                                                  "do",
                                                                  "else",
                                                                  "enum",
                                                                  "export",
                                                                  "extends",
                                                                  "false",
                                                                  "finally",
                                                                  "for",
                                                                  "function",
                                                                  "if",
                                                                  "implements",
                                                                  "import",
                                                                  "in",
                                                                  "instanceof",
                                                                  "interface",
                                                                  "let",
                                                                  "new",
                                                                  "null",
                                                                  "package",
                                                                  "private",
                                                                  "protected",
                                                                  "public",
                                                                  "return",
                                                                  "static",
                                                                  "super",
                                                                  "switch",
                                                                  "this",
                                                                  "throw",
                                                                  "true",
                                                                  "try",
                                                                  "typeof",
                                                                  "var",
                                                                  "void",
                                                                  "while",
                                                                  "with",
                                                                  "yield",
                                                                  "mcp",
                                                                  "help",
                                                                  "input",
                                                                  "globalThis",
                                                                  "Object",
                                                                  "Array",
                                                                  "Promise",
                                                                  "JSON",
                                                                  "Function",
                                                                  "eval",
                                                                  "undefined",
                                                                  "NaN",
                                                                  "Infinity",
                                                                  "__slop_catalog",
                                                                  "global",
                                                                  "console",
                                                                  "Number",
                                                                  "String",
                                                                  "Boolean",
                                                                  "Symbol",
                                                                  "BigInt",
                                                                  "Date",
                                                                  "RegExp",
                                                                  "Error",
                                                                  "TypeError",
                                                                  "Map",
                                                                  "Set",
                                                                  "WeakMap",
                                                                  "WeakSet",
                                                                  "Proxy",
                                                                  "Reflect",
                                                                  "Math",
                                                                  "Atomics",
                                                                  "Intl",
                                                                  "ArrayBuffer",
                                                                  "SharedArrayBuffer",
                                                                  "DataView",
                                                                  "JSONParse",
                                                                  "arguments",
                                                                  "parseInt",
                                                                  "parseFloat",
                                                                  "isFinite",
                                                                  "isNaN",
                                                                  "decodeURI",
                                                                  "decodeURIComponent",
                                                                  "encodeURI",
                                                                  "encodeURIComponent",
                                                                  "WeakRef",
                                                                  "FinalizationRegistry",
                                                                  "AggregateError",
                                                                  "Iterator",
                                                                  "async",
                                                                  "of",
                                                                  "get",
                                                                  "set",
                                                                  "__proto__",
                                                                  "constructor",
                                                                  "prototype",
                                                                  "toString",
                                                                  "toLocaleString",
                                                                  "valueOf",
                                                                  "hasOwnProperty",
                                                                  "isPrototypeOf",
                                                                  "propertyIsEnumerable",
                                                                  "__defineGetter__",
                                                                  "__defineSetter__",
                                                                  "__lookupGetter__",
                                                                  "__lookupSetter__"};
  return kReserved.find(alias) == kReserved.end();
}

absl::Status ValidateJsonObjectFields(const nlohmann::json& value,
                                      std::initializer_list<absl::string_view> allowed_fields,
                                      absl::string_view object_name) {
  if (!value.is_object()) return absl::InvalidArgumentError(absl::StrCat(object_name, " must be an object"));
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    bool allowed = false;
    for (absl::string_view field : allowed_fields) {
      if (iterator.key() == field) {
        allowed = true;
        break;
      }
    }
    if (!allowed)
      return absl::InvalidArgumentError(absl::StrCat(object_name, " has unknown field '", iterator.key(), "'"));
  }
  return absl::OkStatus();
}

}  // namespace slop::mcp
