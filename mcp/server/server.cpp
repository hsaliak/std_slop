#include "mcp/server/server.h"

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"

#include "core/json_utils.h"
#include "mcp/protocol.h"

namespace slop::mcp::server {
namespace {

constexpr char kVersionKey[] = "io.modelcontextprotocol/protocolVersion";
constexpr char kClientInfoKey[] = "io.modelcontextprotocol/clientInfo";
constexpr char kClientCapabilitiesKey[] = "io.modelcontextprotocol/clientCapabilities";
constexpr char kServerInfoKey[] = "io.modelcontextprotocol/serverInfo";
constexpr int64_t kMaxSafeInteger = 9007199254740991LL;

nlohmann::json Error(const nlohmann::json& id, int code, const std::string& message,
                     const nlohmann::json& data = nullptr) {
  nlohmann::json error = {{"code", code}, {"message", message}};
  if (!data.is_null()) error["data"] = data;
  return {{"jsonrpc", std::string(kJsonRpcVersion)}, {"id", id}, {"error", std::move(error)}};
}

bool ValidId(const nlohmann::json& id) {
  if (id.is_string()) return true;
  if (id.is_number_unsigned()) return id.get<uint64_t>() <= static_cast<uint64_t>(kMaxSafeInteger);
  if (id.is_number_integer()) {
    const auto value = id.get<int64_t>();
    return value >= -kMaxSafeInteger && value <= kMaxSafeInteger;
  }
  return false;
}

bool ValidInfo(const nlohmann::json& info) {
  if (!info.is_object()) return false;
  const auto name = json_get<std::string>(info, "name");
  const auto version = json_get<std::string>(info, "version");
  if (!name || name->empty() || !version || version->empty()) return false;
  const auto* title = json_at(info, "title");
  return title == nullptr || title->is_string();
}

}  // namespace

Server::Server(ImplementationInfo identity) : identity_(std::move(identity)) {}

absl::StatusOr<Server> Server::Create(ImplementationInfo identity) {
  if (identity.name.empty() || identity.version.empty()) {
    return absl::InvalidArgumentError("Server identity requires name and version");
  }
  return Server(std::move(identity));
}

std::optional<nlohmann::json> Server::Dispatch(absl::string_view raw) const {
  const auto message = json_parse(std::string(raw));
  if (!message) return Error(nullptr, -32700, "Parse error");
  if (!message->is_object()) return Error(nullptr, -32600, "Invalid request");
  const auto method = json_get<std::string>(*message, "method");
  const auto* id = json_at(*message, "id");
  if (json_get_or(*message, "jsonrpc", std::string{}) != kJsonRpcVersion || !method || method->empty() ||
      json_at(*message, "result") != nullptr || json_at(*message, "error") != nullptr ||
      (id != nullptr && !ValidId(*id))) {
    return Error(nullptr, -32600, "Invalid request");
  }
  // Notifications are not requests and must never elicit a response. The MVP
  // has no notification handlers or side effects.
  if (id == nullptr) return std::nullopt;

  const auto* params = json_at(*message, "params");
  const auto* meta = params != nullptr && params->is_object() ? json_at(*params, "_meta") : nullptr;
  if (meta == nullptr || !meta->is_object()) return Error(*id, -32602, "Request metadata must be an object");
  const auto version = json_get<std::string>(*meta, kVersionKey);
  if (!version || version->empty()) {
    return Error(*id, -32602, "Request must declare protocol version 2026-07-28");
  }
  if (*version != kModernProtocolVersion) {
    return Error(*id, -32022, "Unsupported protocol version",
                 {{"supported", {std::string(kModernProtocolVersion)}}, {"requested", *version}});
  }
  if (const auto* info = json_at(*meta, kClientInfoKey); info != nullptr && !ValidInfo(*info)) {
    return Error(*id, -32602, "Invalid client identity");
  }
  if (const auto* capabilities = json_at(*meta, kClientCapabilitiesKey);
      capabilities != nullptr && !capabilities->is_object()) {
    return Error(*id, -32602, "Client capabilities must be an object");
  }
  if (*method != "server/discover") return Error(*id, -32601, "Method not found");

  nlohmann::json info = {{"name", identity_.name}, {"version", identity_.version}};
  if (identity_.title) info["title"] = *identity_.title;
  return nlohmann::json{{"jsonrpc", std::string(kJsonRpcVersion)},
                        {"id", *id},
                        {"result",
                         {{"resultType", "complete"},
                          {"supportedVersions", {std::string(kModernProtocolVersion)}},
                          {"capabilities", nlohmann::json::object()},
                          {"_meta", {{kServerInfoKey, std::move(info)}}}}}};
}

}  // namespace slop::mcp::server
