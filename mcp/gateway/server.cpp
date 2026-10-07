#include "mcp/gateway/server.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "core/json_utils.h"
#include "mcp/gateway/broker.h"
#include "mcp/protocol.h"

namespace slop::mcp::gateway {
namespace {

std::string ErrorCategory(const absl::Status& status) {
  switch (status.code()) {
    case absl::StatusCode::kDeadlineExceeded:
    case absl::StatusCode::kResourceExhausted:
      return "budget";
    case absl::StatusCode::kNotFound:
      return "catalog";
    case absl::StatusCode::kPermissionDenied:
      return "authorization";
    case absl::StatusCode::kUnavailable:
      return "downstream";
    case absl::StatusCode::kFailedPrecondition:
      return "execution";
    case absl::StatusCode::kInvalidArgument:
      return "javascript";
    default:
      return "internal";
  }
}

mcp::ToolCallResult ActionableError(const absl::Status& status) {
  const std::string message(status.message());
  const std::string bounded = message.substr(0, 4096);
  mcp::ToolCallResult result;
  result.is_error = true;
  result.content.push_back({{"type", "text"}, {"text", bounded}});
  result.structured_content = nlohmann::json::object(
      {{"error", nlohmann::json::object({{"category", ErrorCategory(status)}, {"message", bounded}})}});
  return result;
}

}  // namespace

absl::StatusOr<server::Server> CreateServer(std::shared_ptr<js_runtime::AsyncToolBroker> broker,
                                            js_runtime::RuntimeOptions options, RunJsExecutor executor,
                                            std::shared_ptr<TraceLogger> trace_logger) {
  if (broker == nullptr) return absl::InvalidArgumentError("gateway broker must not be null");
  mcp::ImplementationInfo identity;
  identity.name = "slop-run-js-gateway";
  identity.version = "1.0";

  server::ToolRegistration run_js;
  run_js.definition.name = "run_js";
  run_js.definition.description = "Run bounded JavaScript that composes explicitly authorized downstream MCP tools";
  run_js.definition.input_schema = {
      {"type", "object"},
      {"properties", {{"code", {{"type", "string"}, {"minLength", 1}}}, {"input", {{"type", "object"}}}}},
      {"required", {"code"}},
      {"additionalProperties", false}};
  run_js.handler = [broker, options, executor = std::move(executor), trace_logger = std::move(trace_logger)](
                       const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
    const auto code = json_get<std::string>(arguments, "code");
    if (!code.has_value()) return absl::InvalidArgumentError("run_js requires string field code");
    const nlohmann::json* input = json_at(arguments, "input");
    const nlohmann::json empty_input = nlohmann::json::object();
    const nlohmann::json& script_input = input == nullptr ? empty_input : *input;
    const std::uint64_t trace_id = trace_logger == nullptr ? 0 : trace_logger->BeginRun(*code, script_input);
    auto value = executor ? executor(*code, script_input, broker.get(), options, trace_id)
                          : js_runtime::Run(*code, script_input, broker.get(), options);
    if (!value.ok()) {
      if (trace_logger != nullptr) trace_logger->FailRun(trace_id, value.status());
      return ActionableError(value.status());
    }
    if (trace_logger != nullptr) trace_logger->FinishRun(trace_id, *value);
    mcp::ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", "JavaScript completed"}});
    result.structured_content = nlohmann::json::object({{"result", std::move(*value)}});
    return result;
  };

  server::ToolRegistration help;
  help.definition.name = "run_js_help";
  help.definition.description = "List authorized downstream tools or inspect one tool schema";
  help.definition.input_schema = {{"type", "object"},
                                  {"properties", {{"server", {{"type", "string"}}}, {"tool", {{"type", "string"}}}}},
                                  {"additionalProperties", false}};
  help.handler = [broker](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
    const std::string alias = json_get_or(arguments, "server", std::string());
    const std::string tool = json_get_or(arguments, "tool", std::string());
    auto metadata = broker->Help(alias, tool);
    if (!metadata.ok()) return ActionableError(metadata.status());
    mcp::ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", "Authorized tool catalog"}});
    result.structured_content = std::move(*metadata);
    return result;
  };

  return server::Server::Create(std::move(identity), {std::move(run_js), std::move(help)});
}

}  // namespace slop::mcp::gateway
