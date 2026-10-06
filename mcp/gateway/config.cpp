#include "mcp/gateway/config.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/client/stdio_transport.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCommandBytes = 4096;
constexpr std::size_t kMaxArgCount = 64;
constexpr std::size_t kMaxArgBytes = 4096;
constexpr std::size_t kMaxConfigBytes = 1024 * 1024;

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
  absl::Status root_status = CheckFields(value, {"servers", "runTimeoutMs"}, "config");
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
  config.servers.reserve(servers->size());
  std::unordered_set<std::string> aliases;
  for (const nlohmann::json& entry : *servers) {
    absl::Status entry_status = CheckFields(entry, {"alias", "transport", "command", "args", "allowTools"}, "server");
    if (!entry_status.ok()) return entry_status;
    const auto alias = json_get<std::string>(entry, "alias");
    const auto transport = json_get<std::string>(entry, "transport");
    const auto command = json_get<std::string>(entry, "command");
    const nlohmann::json* args_value = json_at(entry, "args");
    const nlohmann::json* grants_value = json_at(entry, "allowTools");
    if (!alias || !transport || !command || args_value == nullptr) {
      return absl::InvalidArgumentError("server requires alias, transport, command, and args");
    }
    if (!IsSafeServerAlias(*alias))
      return absl::InvalidArgumentError("server alias is not a safe JavaScript identifier");
    if (!aliases.insert(*alias).second) return absl::InvalidArgumentError("server aliases must be unique");
    if (*transport != "stdio") return absl::InvalidArgumentError("only stdio transport is supported in this release");
    if (command->empty() || command->size() > kMaxCommandBytes || !std::filesystem::path(*command).is_absolute()) {
      return absl::InvalidArgumentError("stdio command must be a bounded absolute path");
    }
    auto args = StringArray(*args_value, kMaxArgCount, kMaxArgBytes, "server args");
    if (!args.ok()) return args.status();
    std::vector<std::string> grants;
    if (grants_value != nullptr) {
      auto parsed_grants = StringArray(*grants_value, kMaxAllowedToolsPerServer, 256, "allowTools");
      if (!parsed_grants.ok()) return parsed_grants.status();
      grants = std::move(*parsed_grants);
    }
    absl::Status process_status = mcp::ValidateStdioProcessSpec(*command, *args);
    if (!process_status.ok()) return process_status;
    config.servers.push_back({*alias, *command, std::move(*args), std::move(grants)});
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
