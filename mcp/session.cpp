#include "mcp/session.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "core/json_utils.h"
#include "mcp/json_rpc.h"
#include "mcp/protocol.h"

namespace slop::mcp {
namespace {

bool HasObject(const nlohmann::json& object, const char* key) {
  const auto* value = json_at(object, key);
  return value != nullptr && value->is_object();
}

bool HasListChanged(const nlohmann::json& object, const char* key) {
  const auto* value = json_at(object, key);
  return value != nullptr && value->is_object() && json_get_or(*value, "listChanged", false);
}

}  // namespace

Session::Session(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

absl::Status Session::Initialize(const InitializeOptions& options) {
  if (transport_ == nullptr) return absl::InvalidArgumentError("transport must not be null");
  if (state_ == State::kClosed) return absl::FailedPreconditionError("MCP session is closed");
  if (state_ == State::kInitialized) return absl::OkStatus();
  if (options.client_info.name.empty()) return absl::InvalidArgumentError("client_info.name must not be empty");
  if (options.client_info.version.empty()) return absl::InvalidArgumentError("client_info.version must not be empty");

  if (state_ == State::kCreated) {
    const absl::Status start_status = transport_->Start();
    if (!start_status.ok()) return start_status;
    state_ = State::kStarted;
  }

  options_ = options;
  nlohmann::json client_info = {{"name", options.client_info.name}, {"version", options.client_info.version}};
  if (options.client_info.title.has_value()) client_info["title"] = *options.client_info.title;
  nlohmann::json params = {{"protocolVersion", std::string(kLatestProtocolVersion)},
                           {"capabilities", BuildClientCapabilities(options.capabilities)},
                           {"clientInfo", client_info}};
  auto result_or = SendRequest("initialize", params, options.request_timeout);
  if (!result_or.ok()) return result_or.status();
  const absl::Status parse_status = ParseInitializeResult(*result_or);
  if (!parse_status.ok()) return parse_status;
  transport_->SetProtocolVersion(protocol_version_);
  const absl::Status initialized_status = SendNotification("notifications/initialized");
  if (!initialized_status.ok()) return initialized_status;
  state_ = State::kInitialized;
  return absl::OkStatus();
}

absl::Status Session::Close() {
  if (state_ == State::kClosed) return absl::OkStatus();
  state_ = State::kClosed;
  if (transport_ == nullptr) return absl::OkStatus();
  return transport_->Close();
}

absl::Status Session::Ping() {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  auto result_or = SendRequest("ping", nullptr, options_.request_timeout);
  if (!result_or.ok()) return result_or.status();
  return absl::OkStatus();
}

absl::Status Session::Cancel(const JsonRpcId& request_id, absl::string_view reason) {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  if (std::holds_alternative<std::monostate>(request_id)) {
    return absl::InvalidArgumentError("MCP cancellation request id must not be null");
  }
  nlohmann::json params = {{"requestId", std::holds_alternative<int64_t>(request_id)
                                             ? nlohmann::json(std::get<int64_t>(request_id))
                                             : nlohmann::json(std::get<std::string>(request_id))}};
  if (!reason.empty()) params["reason"] = std::string(reason);
  return SendNotification("notifications/cancelled", params);
}

std::vector<ServerNotification> Session::DrainNotifications() {
  std::vector<ServerNotification> notifications = std::move(notifications_);
  notifications_.clear();
  return notifications;
}

absl::StatusOr<std::vector<Tool>> Session::ListTools() {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  std::vector<Tool> tools;
  std::string cursor;
  do {
    nlohmann::json params = nlohmann::json::object();
    if (!cursor.empty()) params["cursor"] = cursor;
    auto result_or = SendRequest("tools/list", params, options_.request_timeout);
    if (!result_or.ok()) return result_or.status();
    if (!result_or->is_object()) return absl::InvalidArgumentError("tools/list result must be an object");
    const auto tool_items = json_get<nlohmann::json::array_t>(*result_or, "tools");
    if (!tool_items) return absl::InvalidArgumentError("tools/list result missing tools array");
    for (const auto& item : *tool_items) {
      auto tool_or = ParseTool(item);
      if (!tool_or.ok()) return tool_or.status();
      tools.push_back(std::move(*tool_or));
    }
    cursor = json_get_or(*result_or, "nextCursor", std::string{});
  } while (!cursor.empty());
  return tools;
}

absl::StatusOr<ToolCallResult> Session::CallTool(absl::string_view name, const nlohmann::json& arguments) {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  if (name.empty()) return absl::InvalidArgumentError("tool name must not be empty");
  if (!arguments.is_object()) return absl::InvalidArgumentError("tool arguments must be an object");
  nlohmann::json params = {{"name", std::string(name)}, {"arguments", arguments}};
  auto result_or = SendRequest("tools/call", params, options_.request_timeout);
  if (!result_or.ok()) return result_or.status();
  return ParseToolCallResult(*result_or);
}

absl::StatusOr<std::vector<Resource>> Session::ListResources() {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  std::vector<Resource> resources;
  std::string cursor;
  do {
    nlohmann::json params = nlohmann::json::object();
    if (!cursor.empty()) params["cursor"] = cursor;
    auto result_or = SendRequest("resources/list", params, options_.request_timeout);
    if (!result_or.ok()) return result_or.status();
    if (!result_or->is_object()) return absl::InvalidArgumentError("resources/list result must be an object");
    const auto resource_items = json_get<nlohmann::json::array_t>(*result_or, "resources");
    if (!resource_items) return absl::InvalidArgumentError("resources/list result missing resources array");
    for (const auto& item : *resource_items) {
      auto resource_or = ParseResource(item);
      if (!resource_or.ok()) return resource_or.status();
      resources.push_back(std::move(*resource_or));
    }
    cursor = json_get_or(*result_or, "nextCursor", std::string{});
  } while (!cursor.empty());
  return resources;
}

absl::StatusOr<ResourceReadResult> Session::ReadResource(absl::string_view uri) {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  if (uri.empty()) return absl::InvalidArgumentError("resource uri must not be empty");
  auto result_or = SendRequest("resources/read", {{"uri", std::string(uri)}}, options_.request_timeout);
  if (!result_or.ok()) return result_or.status();
  return ParseResourceReadResult(*result_or);
}

absl::StatusOr<std::vector<Prompt>> Session::ListPrompts() {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  std::vector<Prompt> prompts;
  std::string cursor;
  do {
    nlohmann::json params = nlohmann::json::object();
    if (!cursor.empty()) params["cursor"] = cursor;
    auto result_or = SendRequest("prompts/list", params, options_.request_timeout);
    if (!result_or.ok()) return result_or.status();
    if (!result_or->is_object()) return absl::InvalidArgumentError("prompts/list result must be an object");
    const auto prompt_items = json_get<nlohmann::json::array_t>(*result_or, "prompts");
    if (!prompt_items) return absl::InvalidArgumentError("prompts/list result missing prompts array");
    for (const auto& item : *prompt_items) {
      auto prompt_or = ParsePrompt(item);
      if (!prompt_or.ok()) return prompt_or.status();
      prompts.push_back(std::move(*prompt_or));
    }
    cursor = json_get_or(*result_or, "nextCursor", std::string{});
  } while (!cursor.empty());
  return prompts;
}

absl::StatusOr<PromptGetResult> Session::GetPrompt(absl::string_view name, const nlohmann::json& arguments) {
  if (state_ != State::kInitialized) return absl::FailedPreconditionError("MCP session is not initialized");
  if (name.empty()) return absl::InvalidArgumentError("prompt name must not be empty");
  if (!arguments.is_object()) return absl::InvalidArgumentError("prompt arguments must be an object");
  auto result_or = SendRequest("prompts/get", {{"name", std::string(name)}, {"arguments", arguments}},
                               options_.request_timeout);
  if (!result_or.ok()) return result_or.status();
  return ParsePromptGetResult(*result_or);
}

absl::StatusOr<nlohmann::json> Session::SendRequest(absl::string_view method, const nlohmann::json& params,
                                                    absl::Duration timeout) {
  const JsonRpcId id = NextRequestId();
  const absl::Status send_status = transport_->Send(BuildJsonRpcRequest(id, method, params));
  if (!send_status.ok()) return send_status;
  while (true) {
    auto message_or = transport_->Receive(timeout);
    if (!message_or.ok()) return message_or.status();
    const auto method = json_get<std::string>(*message_or, "method");
    if (method.has_value()) {
      const auto* request_id = json_at(*message_or, "id");
      if (request_id != nullptr) {
        if (!request_id->is_number_integer() && !request_id->is_string() && !request_id->is_null()) {
          return absl::InvalidArgumentError("MCP server request id must be null, an integer, or a string");
        }
        const nlohmann::json error = {{"jsonrpc", "2.0"},
                                      {"id", *request_id},
                                      {"error", {{"code", -32601}, {"message", "MCP server requests are unsupported"}}}};
        const absl::Status send_status = transport_->Send(error);
        if (!send_status.ok()) return send_status;
        continue;
      }
      const absl::Status notification_status = HandleNotification(*message_or);
      if (!notification_status.ok()) return notification_status;
      continue;
    }
    auto response_or = ParseJsonRpcResponse(*message_or);
    if (!response_or.ok()) return response_or.status();
    if (JsonRpcIdToString(response_or->id) != JsonRpcIdToString(id)) continue;
    if (response_or->error.has_value()) {
      return absl::UnknownError(absl::StrCat("MCP JSON-RPC error ", response_or->error->code, ": ",
                                            response_or->error->message));
    }
    return *response_or->result;
  }
}

absl::Status Session::SendNotification(absl::string_view method, const nlohmann::json& params) {
  return transport_->Send(BuildJsonRpcNotification(method, params));
}

absl::Status Session::HandleNotification(const nlohmann::json& message) {
  const std::string method = json_get_or(message, "method", std::string{});
  const auto* params = json_at(message, "params");
  const nlohmann::json notification_params = params == nullptr ? nlohmann::json::object() : *params;
  if (!notification_params.is_object()) return absl::InvalidArgumentError("MCP notification params must be an object");

  ServerNotificationKind kind;
  if (method == "notifications/progress") {
    kind = ServerNotificationKind::kProgress;
  } else if (method == "notifications/message") {
    kind = ServerNotificationKind::kLogging;
  } else if (method == "notifications/tools/list_changed") {
    kind = ServerNotificationKind::kToolsListChanged;
  } else if (method == "notifications/resources/list_changed") {
    kind = ServerNotificationKind::kResourcesListChanged;
  } else if (method == "notifications/prompts/list_changed") {
    kind = ServerNotificationKind::kPromptsListChanged;
  } else {
    return absl::OkStatus();
  }
  notifications_.push_back({kind, notification_params});
  return absl::OkStatus();
}

int64_t Session::NextRequestId() { return next_request_id_++; }

nlohmann::json Session::BuildClientCapabilities(const ClientCapabilities& capabilities) {
  nlohmann::json out = nlohmann::json::object();
  if (capabilities.roots) out["roots"] = {{"listChanged", capabilities.roots_list_changed}};
  if (capabilities.sampling) out["sampling"] = nlohmann::json::object();
  if (capabilities.elicitation) out["elicitation"] = nlohmann::json::object();
  if (!capabilities.experimental.empty()) out["experimental"] = capabilities.experimental;
  return out;
}

absl::StatusOr<ServerCapabilities> Session::ParseServerCapabilities(const nlohmann::json& capabilities) {
  if (!capabilities.is_object()) return absl::InvalidArgumentError("initialize capabilities must be an object");
  ServerCapabilities parsed;
  parsed.raw = capabilities;
  parsed.tools = HasObject(capabilities, "tools");
  parsed.tools_list_changed = HasListChanged(capabilities, "tools");
  parsed.resources = HasObject(capabilities, "resources");
  parsed.resources_subscribe = parsed.resources && json_get_or(*json_at(capabilities, "resources"), "subscribe", false);
  parsed.resources_list_changed = HasListChanged(capabilities, "resources");
  parsed.prompts = HasObject(capabilities, "prompts");
  parsed.prompts_list_changed = HasListChanged(capabilities, "prompts");
  parsed.logging = HasObject(capabilities, "logging");
  return parsed;
}

absl::StatusOr<Tool> Session::ParseTool(const nlohmann::json& tool) {
  if (!tool.is_object()) return absl::InvalidArgumentError("tool entry must be an object");
  Tool parsed;
  parsed.name = json_get_or(tool, "name", std::string{});
  if (parsed.name.empty()) return absl::InvalidArgumentError("tool entry missing name");
  parsed.title = json_get<std::string>(tool, "title");
  parsed.description = json_get<std::string>(tool, "description");
  const auto* input_schema = json_at(tool, "inputSchema");
  if (input_schema == nullptr || !input_schema->is_object()) {
    return absl::InvalidArgumentError("tool entry missing inputSchema object");
  }
  parsed.input_schema = *input_schema;
  if (const auto* output_schema = json_at(tool, "outputSchema")) {
    if (!output_schema->is_object()) return absl::InvalidArgumentError("tool outputSchema must be an object");
    parsed.output_schema = *output_schema;
  }
  if (const auto* annotations = json_at(tool, "annotations")) {
    if (!annotations->is_object()) return absl::InvalidArgumentError("tool annotations must be an object");
    parsed.annotations = *annotations;
  }
  return parsed;
}

absl::StatusOr<ToolCallResult> Session::ParseToolCallResult(const nlohmann::json& result) {
  if (!result.is_object()) return absl::InvalidArgumentError("tools/call result must be an object");
  ToolCallResult parsed;
  const auto content = json_get<nlohmann::json::array_t>(result, "content");
  if (!content) return absl::InvalidArgumentError("tools/call result missing content array");
  parsed.content.assign(content->begin(), content->end());
  parsed.is_error = json_get_or(result, "isError", false);
  if (const auto* structured = json_at(result, "structuredContent")) {
    parsed.structured_content = *structured;
  }
  return parsed;
}

absl::StatusOr<Resource> Session::ParseResource(const nlohmann::json& resource) {
  if (!resource.is_object()) return absl::InvalidArgumentError("resource entry must be an object");
  Resource parsed;
  parsed.uri = json_get_or(resource, "uri", std::string{});
  if (parsed.uri.empty()) return absl::InvalidArgumentError("resource entry missing uri");
  parsed.name = json_get_or(resource, "name", std::string{});
  if (parsed.name.empty()) return absl::InvalidArgumentError("resource entry missing name");
  parsed.title = json_get<std::string>(resource, "title");
  parsed.description = json_get<std::string>(resource, "description");
  parsed.mime_type = json_get<std::string>(resource, "mimeType");
  return parsed;
}

absl::StatusOr<ResourceReadResult> Session::ParseResourceReadResult(const nlohmann::json& result) {
  if (!result.is_object()) return absl::InvalidArgumentError("resources/read result must be an object");
  const auto contents = json_get<nlohmann::json::array_t>(result, "contents");
  if (!contents) return absl::InvalidArgumentError("resources/read result missing contents array");
  ResourceReadResult parsed;
  parsed.contents.assign(contents->begin(), contents->end());
  return parsed;
}

absl::StatusOr<Prompt> Session::ParsePrompt(const nlohmann::json& prompt) {
  if (!prompt.is_object()) return absl::InvalidArgumentError("prompt entry must be an object");
  Prompt parsed;
  parsed.name = json_get_or(prompt, "name", std::string{});
  if (parsed.name.empty()) return absl::InvalidArgumentError("prompt entry missing name");
  parsed.title = json_get<std::string>(prompt, "title");
  parsed.description = json_get<std::string>(prompt, "description");
  if (const auto* arguments = json_at(prompt, "arguments")) {
    if (!arguments->is_array()) return absl::InvalidArgumentError("prompt arguments must be an array");
    parsed.arguments = *arguments;
  }
  return parsed;
}

absl::StatusOr<PromptGetResult> Session::ParsePromptGetResult(const nlohmann::json& result) {
  if (!result.is_object()) return absl::InvalidArgumentError("prompts/get result must be an object");
  const auto messages = json_get<nlohmann::json::array_t>(result, "messages");
  if (!messages) return absl::InvalidArgumentError("prompts/get result missing messages array");
  PromptGetResult parsed;
  parsed.description = json_get<std::string>(result, "description");
  parsed.messages.assign(messages->begin(), messages->end());
  return parsed;
}

absl::Status Session::ParseInitializeResult(const nlohmann::json& result) {
  if (!result.is_object()) return absl::InvalidArgumentError("initialize result must be an object");
  const std::string version = json_get_or(result, "protocolVersion", std::string{});
  if (version.empty()) return absl::InvalidArgumentError("initialize result missing protocolVersion");
  if (version != kLatestProtocolVersion) return absl::UnimplementedError(absl::StrCat("Unsupported MCP protocol version: ", version));
  const auto* capabilities = json_at(result, "capabilities");
  if (capabilities == nullptr) return absl::InvalidArgumentError("initialize result missing capabilities");
  auto capabilities_or = ParseServerCapabilities(*capabilities);
  if (!capabilities_or.ok()) return capabilities_or.status();
  protocol_version_ = version;
  server_capabilities_ = *capabilities_or;
  return absl::OkStatus();
}

}  // namespace slop::mcp
