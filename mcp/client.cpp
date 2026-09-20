#include "mcp/client.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/modern.h"
#include "mcp/modern_http.h"
#include "mcp/protocol.h"
#include "mcp/session.h"
#include "mcp/streamable_http_transport.h"

namespace slop::mcp {
namespace {

absl::Status FailureStatus(const ProtocolFailure& failure) {
  const std::string message = failure.json_rpc_code
                                  ? absl::StrCat(failure.message, " (JSON-RPC ", *failure.json_rpc_code, ")")
                                  : failure.message;
  if (failure.http_status == 401) return absl::UnauthenticatedError(message);
  if (failure.http_status == 403) return absl::PermissionDeniedError(message);
  if (failure.http_status == 404 || failure.json_rpc_code == -32601) {
    return absl::UnimplementedError(message);
  }
  if (failure.execution == ExecutionCertainty::kMayHaveExecuted) {
    return absl::AbortedError(absl::StrCat(message, "; operation outcome is unknown"));
  }
  return absl::FailedPreconditionError(message);
}

v2025_11_25::InitializeOptions ClassicOptions(const ClientOptions& options, const StreamableHttpConfig& config) {
  v2025_11_25::InitializeOptions classic;
  classic.client_info = options.client_info;
  classic.capabilities = options.classic_capabilities;
  classic.request_timeout = config.request_timeout;
  return classic;
}

class ClassicClient final : public Client {
 public:
  explicit ClassicClient(std::unique_ptr<v2025_11_25::Session> session) : session_(std::move(session)) {}

  ProtocolRevision revision() const override { return ProtocolRevision::k2025_11_25; }
  absl::StatusOr<std::vector<Tool>> ListTools() override { return session_->ListTools(); }
  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) override {
    return session_->CallTool(name, arguments);
  }
  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string&, const nlohmann::json&,
                                                  const nlohmann::json&) override {
    return absl::UnimplementedError("tool continuations require modern MCP");
  }

 private:
  std::unique_ptr<v2025_11_25::Session> session_;
};

class ModernClient final : public Client {
 public:
  ModernClient(v2026_07_28::HttpExchangeOptions exchange_options, v2026_07_28::RequestContext request_context,
               HttpClient* http_client)
      : exchange_(std::move(exchange_options), http_client), request_context_(std::move(request_context)) {}

  ProtocolRevision revision() const override { return ProtocolRevision::k2026_07_28; }

  absl::StatusOr<std::vector<Tool>> ListTools() override {
    std::vector<Tool> catalog;
    absl::flat_hash_set<std::string> tool_names;
    absl::flat_hash_set<std::string> cursors;
    std::optional<std::string> cursor;
    for (size_t page = 0; page < kMaxCatalogPages; ++page) {
      v2026_07_28::Request request = MakeRequest("tools/list");
      if (cursor) request.params["cursor"] = *cursor;
      auto exchange_or = exchange_.Execute(request);
      if (!exchange_or.ok()) return exchange_or.status();
      if (exchange_or->failure) return FailureStatus(*exchange_or->failure);
      if (!exchange_or->response || !exchange_or->response->result) {
        return absl::InvalidArgumentError("tools/list missing result");
      }
      const nlohmann::json& result = *exchange_or->response->result;
      auto page_tools_or = v2026_07_28::ParseToolsList(result);
      if (!page_tools_or.ok()) return page_tools_or.status();
      for (Tool& tool : *page_tools_or) {
        if (!tool_names.insert(tool.name).second) {
          return absl::InvalidArgumentError(absl::StrCat("duplicate MCP tool name: ", tool.name));
        }
        if (catalog.size() >= kMaxCatalogTools) {
          return absl::ResourceExhaustedError("MCP catalog tool limit exceeded");
        }
        catalog.push_back(std::move(tool));
      }
      const nlohmann::json* next_cursor_value = json_at(result, "nextCursor");
      if (next_cursor_value != nullptr && !next_cursor_value->is_string()) {
        return absl::InvalidArgumentError("tools/list nextCursor must be a string");
      }
      const auto next_cursor = json_get<std::string>(result, "nextCursor");
      if (!next_cursor || next_cursor->empty()) {
        tools_.clear();
        for (const Tool& tool : catalog) tools_[tool.name] = tool;
        return catalog;
      }
      if (!cursors.insert(*next_cursor).second) {
        return absl::InvalidArgumentError("MCP catalog cursor cycle");
      }
      cursor = *next_cursor;
    }
    return absl::ResourceExhaustedError("MCP catalog page limit exceeded");
  }

  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) override {
    return ExecuteToolCall(name, arguments, nullptr);
  }

  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string& name, const nlohmann::json& arguments,
                                                  const nlohmann::json& request_state) override {
    return ExecuteToolCall(name, arguments, &request_state);
  }

 private:
  absl::StatusOr<ToolCallResult> ExecuteToolCall(const std::string& name, const nlohmann::json& arguments,
                                                 const nlohmann::json* request_state) {
    const auto tool = tools_.find(name);
    if (tool == tools_.end()) {
      return absl::FailedPreconditionError("tools/list must provide a valid tool before tools/call");
    }
    const absl::Status validation = v2026_07_28::ValidateToolArguments(tool->second, arguments);
    if (!validation.ok()) return validation;
    v2026_07_28::Request request = MakeRequest("tools/call");
    request.params = {{"name", name}, {"arguments", arguments}};
    if (request_state != nullptr) request.params["requestState"] = *request_state;
    request.tool_schema = tool->second.input_schema;
    auto exchange_or = exchange_.Execute(request);
    if (!exchange_or.ok()) return exchange_or.status();
    if (exchange_or->failure) return FailureStatus(*exchange_or->failure);
    if (!exchange_or->response || !exchange_or->response->result) {
      return absl::InvalidArgumentError("tools/call missing result");
    }
    const nlohmann::json* output_schema = tool->second.output_schema.empty() ? nullptr : &tool->second.output_schema;
    return v2026_07_28::ParseToolCallResult(*exchange_or->response->result, output_schema);
  }

  v2026_07_28::Request MakeRequest(absl::string_view method) {
    v2026_07_28::Request request;
    request.id = absl::StrCat("modern-", next_request_id_++);
    request.method = std::string(method);
    request.context = request_context_;
    return request;
  }

  v2026_07_28::HttpExchange exchange_;
  v2026_07_28::RequestContext request_context_;
  int64_t next_request_id_ = 2;
  absl::flat_hash_map<std::string, Tool> tools_;
};

absl::StatusOr<std::unique_ptr<Client>> ConnectClassic(const StreamableHttpConfig& config, const ClientOptions& options,
                                                       HttpClient* http_client) {
  auto session_or = ConnectClassicStreamableHttp(config, ClassicOptions(options, config), http_client);
  if (!session_or.ok()) return session_or.status();
  return std::unique_ptr<Client>(std::make_unique<ClassicClient>(std::move(*session_or)));
}

v2026_07_28::HttpExchangeOptions ModernHttpOptions(const StreamableHttpConfig& config) {
  v2026_07_28::HttpExchangeOptions options;
  options.endpoint_url = config.endpoint_url;
  options.extra_headers = config.extra_headers;
  options.bearer_token = config.bearer_token;
  options.deadline = config.request_timeout;
  return options;
}

}  // namespace

absl::StatusOr<std::unique_ptr<v2025_11_25::Session>> ConnectClassicStreamableHttp(
    const StreamableHttpConfig& config, const v2025_11_25::InitializeOptions& options, HttpClient* http_client) {
  if (http_client == nullptr) {
    return absl::InvalidArgumentError("http_client must not be null");
  }
  auto transport = std::make_unique<v2025_11_25::StreamableHttpTransport>(config, http_client);
  auto session = std::make_unique<v2025_11_25::Session>(std::move(transport));
  const absl::Status status = session->Initialize(options);
  if (!status.ok()) return status;
  return session;
}

absl::StatusOr<std::unique_ptr<Client>> ConnectMcp(const StreamableHttpConfig& config, const ClientOptions& options,
                                                   HttpClient* http_client) {
  if (http_client == nullptr) {
    return absl::InvalidArgumentError("http_client must not be null");
  }
  if (options.client_info.name.empty() || options.client_info.version.empty()) {
    return absl::InvalidArgumentError("client info requires name and version");
  }
  if (options.selection == SelectionPolicy::kClassicOnly) {
    return ConnectClassic(config, options, http_client);
  }

  v2026_07_28::RequestContext context;
  context.client_info = options.client_info;
  context.client_capabilities = options.modern_capabilities;
  v2026_07_28::HttpExchange exchange(ModernHttpOptions(config), http_client);
  v2026_07_28::Request discovery_request;
  discovery_request.id = std::string("modern-1");
  discovery_request.method = "server/discover";
  discovery_request.context = context;
  auto exchange_or = exchange.Execute(discovery_request);
  if (!exchange_or.ok()) return exchange_or.status();
  if (exchange_or->failure) {
    const ProtocolFailure& failure = *exchange_or->failure;
    const bool unrecognized_bad_request = failure.http_status == 400 && !failure.json_rpc_code.has_value();
    const bool unrecognized_method = failure.http_status == 200 && failure.json_rpc_code == -32601;
    if (options.selection == SelectionPolicy::kPreferLatest &&
        (unrecognized_bad_request || unrecognized_method)) {
      return ConnectClassic(config, options, http_client);
    }
    return FailureStatus(failure);
  }
  if (!exchange_or->response || !exchange_or->response->result) {
    return absl::InvalidArgumentError("server/discover missing result");
  }
  auto discovery_or = v2026_07_28::ParseDiscovery(*exchange_or->response->result);
  if (!discovery_or.ok()) return discovery_or.status();
  bool supports_modern = false;
  for (const std::string& version : discovery_or->supported_versions) {
    supports_modern = supports_modern || version == kModernProtocolVersion;
  }
  if (!supports_modern) {
    return absl::UnimplementedError("server/discover did not select MCP 2026-07-28");
  }
  return std::unique_ptr<Client>(
      std::make_unique<ModernClient>(ModernHttpOptions(config), std::move(context), http_client));
}

}  // namespace slop::mcp
