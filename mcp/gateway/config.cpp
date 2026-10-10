#include "mcp/gateway/config.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/client/stdio_transport.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCommandBytes = 4096;
constexpr std::size_t kMaxEndpointUrlBytes = 4096;
constexpr std::size_t kMaxArgCount = 64;
constexpr std::size_t kMaxArgBytes = 4096;
constexpr std::size_t kMaxConfigBytes = 1024 * 1024;
constexpr std::size_t kMaxTraceLogPathBytes = 4096;

bool IsSafeAlias(const std::string& alias) {
  if (alias.empty() || alias.size() > 64 ||
      !((alias[0] >= 'A' && alias[0] <= 'Z') || (alias[0] >= 'a' && alias[0] <= 'z') || alias[0] == '_')) {
    return false;
  }
  for (char c : alias) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) {
      return false;
    }
  }
  static const std::unordered_set<std::string> kReserved = {"await",
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
                                                            "Date",
                                                            "RegExp",
                                                            "Error",
                                                            "TypeError",
                                                            "BigInt",
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

absl::Status CheckFields(const nlohmann::json& value, const std::unordered_set<std::string>& allowed,
                         const std::string& path) {
  if (!value.is_object()) return absl::InvalidArgumentError(absl::StrCat(path, " must be an object"));
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (allowed.find(it.key()) == allowed.end())
      return absl::InvalidArgumentError(absl::StrCat(path, " has unknown field '", it.key(), "'"));
  }
  return absl::OkStatus();
}

bool IsValidPort(const std::string& port) {
  if (port.empty()) return false;
  unsigned int value = 0;
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  return parsed.ec == std::errc() && parsed.ptr == port.data() + port.size() && value > 0 && value <= 65535;
}

bool IsValidRegName(const std::string& host) {
  if (host.empty()) return false;
  constexpr std::string_view kSubDelimiters = "!$&'()*+,;=";
  for (std::size_t i = 0; i < host.size(); ++i) {
    const unsigned char c = host[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~' || kSubDelimiters.find(static_cast<char>(c)) != std::string_view::npos) {
      continue;
    }
    if (c != '%' || i + 2 >= host.size()) return false;
    const auto is_hex = [](unsigned char digit) {
      return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
    };
    if (!is_hex(host[i + 1]) || !is_hex(host[i + 2])) return false;
    i += 2;
  }
  return true;
}

bool IsValidIpv4Address(std::string_view address) {
  std::size_t octet_count = 0;
  while (!address.empty()) {
    const std::size_t separator = address.find('.');
    const std::string_view octet = address.substr(0, separator);
    unsigned int value = 0;
    if (octet.empty() || (octet.size() > 1 && octet.front() == '0')) return false;
    const auto parsed = std::from_chars(octet.data(), octet.data() + octet.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != octet.data() + octet.size() || value > 255) return false;
    ++octet_count;
    if (separator == std::string_view::npos) break;
    if (separator + 1 == address.size()) return false;
    address.remove_prefix(separator + 1);
  }
  return octet_count == 4;
}

bool IsValidIpv6Part(std::string_view part, std::size_t* group_count, bool allow_ipv4_tail) {
  while (!part.empty()) {
    const std::size_t separator = part.find(':');
    const std::string_view group = part.substr(0, separator);
    if (group.find('.') != std::string_view::npos) {
      if (!allow_ipv4_tail || separator != std::string_view::npos || !IsValidIpv4Address(group)) return false;
      *group_count += 2;
      return true;
    }
    if (group.empty() || group.size() > 4) return false;
    for (unsigned char c : group) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    ++*group_count;
    if (separator == std::string_view::npos) break;
    if (separator + 1 == part.size()) return false;
    part.remove_prefix(separator + 1);
  }
  return true;
}

bool IsValidIpv6Address(std::string_view address) {
  const std::size_t compression = address.find("::");
  if (address.find(":::") != std::string_view::npos) return false;
  std::size_t groups = 0;
  if (compression == std::string_view::npos) {
    return IsValidIpv6Part(address, &groups, true) && groups == 8;
  }
  if (address.find("::", compression + 2) != std::string_view::npos) return false;
  const std::string_view prefix = address.substr(0, compression);
  const std::string_view suffix = address.substr(compression + 2);
  return IsValidIpv6Part(prefix, &groups, false) && IsValidIpv6Part(suffix, &groups, true) && groups < 8;
}

bool IsValidAuthority(const std::string& authority) {
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos || close == 1) return false;
    const std::string address = authority.substr(1, close - 1);
    if (!IsValidIpv6Address(address)) return false;
    const std::string suffix = authority.substr(close + 1);
    return suffix.empty() || (suffix.front() == ':' && IsValidPort(suffix.substr(1)));
  }
  if (authority.find('[') != std::string::npos || authority.find(']') != std::string::npos) return false;
  const std::size_t colon = authority.find(':');
  const std::string host = authority.substr(0, colon);
  const bool numeric_dotted_host =
      host.find('.') != std::string::npos &&
      std::all_of(host.begin(), host.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; });
  if ((numeric_dotted_host && !IsValidIpv4Address(host)) || !IsValidRegName(host) ||
      (colon != std::string::npos && authority.find(':', colon + 1) != std::string::npos)) {
    return false;
  }
  return colon == std::string::npos || IsValidPort(authority.substr(colon + 1));
}

bool IsValidHttpEndpoint(const std::string& url) {
  if (url.empty() || url.size() > kMaxEndpointUrlBytes || url.find('\0') != std::string::npos ||
      url.find('#') != std::string::npos) {
    return false;
  }
  std::size_t authority_start = 0;
  if (url.rfind("https://", 0) == 0) {
    authority_start = 8;
  } else if (url.rfind("http://", 0) == 0) {
    authority_start = 7;
  } else {
    return false;
  }
  const std::size_t authority_end = url.find_first_of("/?", authority_start);
  const std::string authority = url.substr(authority_start, authority_end - authority_start);
  if (!IsValidAuthority(authority)) return false;
  for (std::size_t i = 0; i < url.size(); ++i) {
    const unsigned char c = url[i];
    if (c <= 0x20 || c == 0x7f) return false;
    if (c == '%') {
      if (i + 2 >= url.size()) return false;
      const auto is_hex = [](unsigned char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
      };
      if (!is_hex(url[i + 1]) || !is_hex(url[i + 2])) return false;
      i += 2;
    }
  }
  return true;
}

absl::StatusOr<std::vector<std::string>> StringArray(const nlohmann::json& value, std::size_t max_count,
                                                     std::size_t max_item_bytes, const std::string& path) {
  if (!value.is_array() || value.size() > max_count) {
    return absl::InvalidArgumentError(absl::StrCat(path, " must be a bounded array"));
  }
  std::vector<std::string> result;
  result.reserve(value.size());
  std::unordered_set<std::string> unique;
  for (const auto& item : value) {
    if (!item.is_string()) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be strings"));
    const auto parsed_text = json_getter<std::string>::get(item);
    if (!parsed_text.has_value()) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be strings"));
    const std::string& text = *parsed_text;
    if (text.empty() || text.size() > max_item_bytes) {
      return absl::InvalidArgumentError(absl::StrCat(path, " entries must be non-empty and bounded"));
    }
    if (!unique.insert(text).second) return absl::InvalidArgumentError(absl::StrCat(path, " entries must be unique"));
    result.push_back(text);
  }
  return result;
}

}  // namespace

bool IsSafeServerAlias(const std::string& alias) { return IsSafeAlias(alias); }

absl::StatusOr<GatewayConfig> ParseConfig(const nlohmann::json& value) {
  absl::Status root_status = CheckFields(value, {"servers", "runTimeoutMs", "traceLogPath"}, "config");
  if (!root_status.ok()) return root_status;
  const nlohmann::json* servers = json_at(value, "servers");
  if (servers == nullptr || !servers->is_array() || servers->size() > kMaxServers) {
    return absl::InvalidArgumentError("config servers must be a bounded array");
  }

  GatewayConfig config;
  if (json_at(value, "runTimeoutMs") != nullptr) {
    const auto timeout = json_get<std::int64_t>(value, "runTimeoutMs");
    if (!timeout || *timeout < 1 || *timeout > 60'000) {
      return absl::InvalidArgumentError("config runTimeoutMs must be between 1 and 60000");
    }
    config.run_timeout_ms = *timeout;
  }
  if (json_at(value, "traceLogPath") != nullptr) {
    const auto trace_log_path = json_get<std::string>(value, "traceLogPath");
    if (!trace_log_path || trace_log_path->empty() || trace_log_path->size() > kMaxTraceLogPathBytes ||
        trace_log_path->find('\0') != std::string::npos || !std::filesystem::path(*trace_log_path).is_absolute()) {
      return absl::InvalidArgumentError("config traceLogPath must be a bounded absolute path");
    }
    config.trace_log_path = *trace_log_path;
  }
  config.servers.reserve(servers->size());
  std::unordered_set<std::string> aliases;
  for (const nlohmann::json& entry : *servers) {
    absl::Status entry_status =
        CheckFields(entry, {"alias", "transport", "command", "args", "endpointUrl", "allowTools"}, "server");
    if (!entry_status.ok()) return entry_status;
    const auto alias = json_get<std::string>(entry, "alias");
    const auto transport = json_get<std::string>(entry, "transport");
    const nlohmann::json* grants_value = json_at(entry, "allowTools");
    if (!alias || !transport) return absl::InvalidArgumentError("server requires alias and transport");
    if (!IsSafeServerAlias(*alias))
      return absl::InvalidArgumentError("server alias is not a safe JavaScript identifier");
    if (!aliases.insert(*alias).second) return absl::InvalidArgumentError("server aliases must be unique");

    ServerConfig server;
    server.alias = *alias;
    if (grants_value != nullptr) {
      auto parsed_grants = StringArray(*grants_value, kMaxAllowedToolsPerServer, 256, "allowTools");
      if (!parsed_grants.ok()) return parsed_grants.status();
      server.allow_tools = std::move(*parsed_grants);
    }

    if (*transport == "stdio") {
      if (json_at(entry, "endpointUrl") != nullptr) {
        return absl::InvalidArgumentError("stdio server must not include endpointUrl");
      }
      const auto command = json_get<std::string>(entry, "command");
      const nlohmann::json* args_value = json_at(entry, "args");
      if (!command || args_value == nullptr) {
        return absl::InvalidArgumentError("stdio server requires command and args");
      }
      if (command->empty() || command->size() > kMaxCommandBytes || !std::filesystem::path(*command).is_absolute()) {
        return absl::InvalidArgumentError("stdio command must be a bounded absolute path");
      }
      auto args = StringArray(*args_value, kMaxArgCount, kMaxArgBytes, "server args");
      if (!args.ok()) return args.status();
      absl::Status process_status = mcp::ValidateStdioProcessSpec(*command, *args);
      if (!process_status.ok()) return process_status;
      server.transport = ServerTransport::kStdio;
      server.command = *command;
      server.args = std::move(*args);
    } else if (*transport == "http") {
      if (json_at(entry, "command") != nullptr || json_at(entry, "args") != nullptr) {
        return absl::InvalidArgumentError("http server must not include command or args");
      }
      const auto endpoint_url = json_get<std::string>(entry, "endpointUrl");
      if (!endpoint_url || !IsValidHttpEndpoint(*endpoint_url)) {
        return absl::InvalidArgumentError("http server endpointUrl must be a valid bounded HTTP(S) URL");
      }
      server.transport = ServerTransport::kHttp;
      server.endpoint_url = *endpoint_url;
    } else {
      return absl::InvalidArgumentError("server transport must be stdio or http");
    }
    config.servers.push_back(std::move(server));
  }
  return config;
}

absl::StatusOr<GatewayConfig> ParseConfigText(const std::string& text) {
  if (text.size() > kMaxConfigBytes) return absl::ResourceExhaustedError("config exceeds maximum size");
  const auto parsed = json_parse(text);
  if (!parsed.has_value()) return absl::InvalidArgumentError("config is not valid JSON");
  return ParseConfig(*parsed);
}

}  // namespace slop::mcp::gateway
