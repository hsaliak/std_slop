#include "mcp/server/server.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"

#include "core/json_utils.h"
#include "mcp/json_schema.h"
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

nlohmann::json Success(const nlohmann::json& id, nlohmann::json result) {
  result["resultType"] = "complete";
  return {{"jsonrpc", std::string(kJsonRpcVersion)}, {"id", id}, {"result", std::move(result)}};
}

bool ValidToolName(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  for (const unsigned char c : name) {
    if (!(absl::ascii_isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

absl::Status ValidateDefinition(const Tool& tool) {
  if (!ValidToolName(tool.name)) return absl::InvalidArgumentError("Invalid tool name");
  if (!tool.input_schema.is_object() || !tool.output_schema.is_object() || !tool.annotations.is_object() ||
      !tool.meta.is_object()) {
    return absl::InvalidArgumentError("Tool schemas, annotations and metadata must be objects");
  }
  if (const auto* type = json_at(tool.input_schema, "type");
      type != nullptr && json_get_or(tool.input_schema, "type", std::string{}) != "object") {
    return absl::InvalidArgumentError("Tool input schema must describe an object");
  }
  const auto input_status = CheckJsonSchema(tool.input_schema);
  if (!input_status.ok()) return input_status;
  return CheckJsonSchema(tool.output_schema);
}

nlohmann::json EncodeDefinition(const Tool& tool) {
  nlohmann::json value = {{"name", tool.name}, {"inputSchema", tool.input_schema}};
  if (tool.title) value["title"] = *tool.title;
  if (tool.description) value["description"] = *tool.description;
  if (!tool.output_schema.empty()) value["outputSchema"] = tool.output_schema;
  if (!tool.annotations.empty()) value["annotations"] = tool.annotations;
  if (!tool.meta.empty()) value["_meta"] = tool.meta;
  return value;
}

bool HasString(const nlohmann::json& object, const char* key) { return json_get<std::string>(object, key).has_value(); }

bool ValidContent(const nlohmann::json& block) {
  if (!block.is_object()) return false;
  for (const char* key : {"annotations", "_meta"}) {
    if (const auto* value = json_at(block, key); value != nullptr && !value->is_object()) return false;
  }
  const auto type = json_get_or(block, "type", std::string{});
  if (type == "text") return HasString(block, "text");
  if (type == "image" || type == "audio") return HasString(block, "data") && HasString(block, "mimeType");
  if (type == "resource_link") return HasString(block, "uri") && HasString(block, "name");
  if (type == "resource") {
    const auto* resource = json_at(block, "resource");
    if (resource == nullptr || !resource->is_object() || !HasString(*resource, "uri")) return false;
    const auto* text = json_at(*resource, "text");
    const auto* blob = json_at(*resource, "blob");
    return (text != nullptr && text->is_string() && blob == nullptr) ||
           (blob != nullptr && blob->is_string() && text == nullptr);
  }
  return false;
}

absl::Status ValidateResult(const Tool& tool, const ToolCallResult& result) {
  if (result.kind != ToolResultKind::kComplete || result.request_state || !result.meta.is_object()) {
    return absl::InvalidArgumentError("Only complete tool results with object metadata are supported");
  }
  for (const auto& block : result.content) {
    if (!ValidContent(block)) return absl::InvalidArgumentError("Invalid tool content block");
  }
  // Tool failures may omit structured output; success must satisfy its schema.
  if (!tool.output_schema.empty() && (!result.is_error || result.structured_content)) {
    if (!result.structured_content) return absl::InvalidArgumentError("Missing structured tool output");
    return ValidateJsonSchema(tool.output_schema, *result.structured_content);
  }
  return absl::OkStatus();
}

nlohmann::json EncodeResult(const ToolCallResult& result) {
  nlohmann::json value = {{"content", result.content}, {"isError", result.is_error}};
  if (result.structured_content) value["structuredContent"] = *result.structured_content;
  if (!result.meta.empty()) value["_meta"] = result.meta;
  return value;
}

nlohmann::json InvalidArguments(const nlohmann::json& id, const absl::Status& status) {
  ToolCallResult result;
  result.is_error = true;
  result.content.push_back({{"type", "text"}, {"text", std::string(status.message())}});
  return Success(id, EncodeResult(result));
}

}  // namespace

Server::Server(ImplementationInfo identity, std::map<std::string, ToolRegistration> tools)
    : identity_(std::move(identity)), tools_(std::move(tools)) {}

absl::StatusOr<Server> Server::Create(ImplementationInfo identity, std::vector<ToolRegistration> tools) {
  if (identity.name.empty() || identity.version.empty()) {
    return absl::InvalidArgumentError("Server identity requires name and version");
  }
  std::map<std::string, ToolRegistration> registry;
  for (auto& registration : tools) {
    const auto status = ValidateDefinition(registration.definition);
    if (!status.ok()) return status;
    if (!registration.handler) return absl::InvalidArgumentError("Tool handler is empty");
    const std::string name = registration.definition.name;
    if (!registry.emplace(name, std::move(registration)).second) {
      return absl::InvalidArgumentError("Duplicate tool name");
    }
  }
  return Server(std::move(identity), std::move(registry));
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
  // Notifications must never elicit a response or execute tool handlers.
  // This implementation has no notification handlers.
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
  if (*method == "tools/list") {
    if (json_at(*params, "cursor") != nullptr) return Error(*id, -32602, "Pagination is not supported");
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& entry : tools_) tools.push_back(EncodeDefinition(entry.second.definition));
    return Success(*id, {{"tools", std::move(tools)}});
  }
  if (*method == "tools/call") {
    const auto name = json_get<std::string>(*params, "name");
    const auto* arguments = json_at(*params, "arguments");
    if (!name || name->empty() || (arguments != nullptr && !arguments->is_object())) {
      return Error(*id, -32602, "Tool call requires a name and object arguments");
    }
    if (json_at(*params, "inputResponses") != nullptr || json_at(*params, "requestState") != nullptr) {
      return Error(*id, -32602, "Resumable calls are not supported");
    }
    const auto tool = tools_.find(*name);
    if (tool == tools_.end()) return Error(*id, -32602, "Unknown tool");
    const auto args = arguments == nullptr ? nlohmann::json::object() : *arguments;
    const auto status = ValidateJsonSchema(tool->second.definition.input_schema, args);
    if (!status.ok()) return InvalidArguments(*id, status);
    const auto result = tool->second.handler(args);
    if (!result.ok()) return Error(*id, -32603, "Tool handler failed");
    if (!ValidateResult(tool->second.definition, *result).ok()) {
      return Error(*id, -32603, "Tool handler returned an invalid result");
    }
    return Success(*id, EncodeResult(*result));
  }
  if (*method != "server/discover") return Error(*id, -32601, "Method not found");

  nlohmann::json info = {{"name", identity_.name}, {"version", identity_.version}};
  if (identity_.title) info["title"] = *identity_.title;
  return nlohmann::json{{"jsonrpc", std::string(kJsonRpcVersion)},
                        {"id", *id},
                        {"result",
                         {{"resultType", "complete"},
                          {"supportedVersions", {std::string(kModernProtocolVersion)}},
                          {"capabilities", {{"tools", nlohmann::json::object()}}},
                          {"_meta", {{kServerInfoKey, std::move(info)}}}}}};
}

}  // namespace slop::mcp::server
