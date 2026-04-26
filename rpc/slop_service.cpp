#include "rpc/slop_service.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "core/status_macros.h"
#include "rpc/execution_policy.h"
#include "grpcpp/grpcpp.h"
#include "rpc/request_validation.h"
#include "tools/tool_executor.h"

namespace slop::rpc::v1 {
namespace {

std::string StatusCodeName(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case absl::StatusCode::kNotFound:
      return "NOT_FOUND";
    case absl::StatusCode::kUnauthenticated:
      return "UNAUTHENTICATED";
    case absl::StatusCode::kPermissionDenied:
      return "PERMISSION_DENIED";
    default:
      return "INTERNAL";
  }
}

grpc::Status ToGrpcStatus(const absl::Status& status) {
  if (status.ok()) {
    return grpc::Status::OK;
  }
  if (status.code() == absl::StatusCode::kInvalidArgument) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::string(status.message()));
  }
  return grpc::Status(grpc::StatusCode::INTERNAL, std::string(status.message()));
}

}  // namespace

SlopServiceImpl::SlopServiceImpl(ServerRuntimeConfig server_config, slop::RuntimeBootstrap runtime,
                                 slop::Database* db)
    : server_config_(std::move(server_config)),
      runtime_(std::move(runtime)),
      db_(db),
      configured_model_(server_config_.runtime_options.model) {}

grpc::Status SlopServiceImpl::RunPrompt(grpc::ServerContext*, const RunPromptRequest* request,
                                        RunPromptResponse* response) {
  auto validated_or = ValidateRunPromptRequest(*request, server_config_);
  if (!validated_or.ok()) {
    response->set_success(false);
    response->set_error_code(StatusCodeName(validated_or.status().code()));
    response->set_error_message(std::string(validated_or.status().message()));
    return ToGrpcStatus(validated_or.status());
  }

  ValidatedRunPromptRequest validated = std::move(*validated_or);
  std::string session_id = validated.session_id.empty() ? "rpc" : validated.session_id;
  const std::string model = validated.model_override.value_or(configured_model_);
  if (runtime_.orchestrator->GetModel() != model) {
    runtime_.orchestrator->Update().WithModel(model).BuildInto(runtime_.orchestrator.get());
  }
  runtime_.engine_config->is_batch_mode = true;
  runtime_.engine_config->silent = true;
  if (validated.context_window.has_value()) {
    absl::Status status = db_->SetContextWindow(session_id, *validated.context_window);
    if (!status.ok()) {
      response->set_success(false);
      response->set_error_code(StatusCodeName(status.code()));
      response->set_error_message(std::string(status.message()));
      return ToGrpcStatus(status);
    }
  }
  runtime_.engine_config->google_api_key = server_config_.runtime_options.google_api_key;
  runtime_.engine_config->openai_api_key = server_config_.runtime_options.openai_api_key;
  runtime_.engine_config->openai_base_url = server_config_.runtime_options.openai_base_url;
  runtime_.engine_config->openai_oauth = server_config_.runtime_options.openai_oauth;
  runtime_.engine_config->use_responses = server_config_.runtime_options.use_responses;
  ApplyServerExecutionPolicy(*runtime_.engine_config, server_config_);

  auto result_or = runtime_.engine->Query(validated.prompt, *runtime_.engine_config, validated.active_skills);

  response->set_success(result_or.ok());
  response->set_content(result_or.ok() ? *result_or : "");
  response->set_session_id(session_id);
  if (!result_or.ok()) {
    response->set_error_code(StatusCodeName(result_or.status().code()));
    response->set_error_message(std::string(result_or.status().message()));
    return ToGrpcStatus(result_or.status());
  }
  return grpc::Status::OK;
}

void ConfigureRpcOpenAiOAuthHandler(slop::HttpClient* http_client,
                                    std::shared_ptr<slop::OAuthHandler>* handler) {
  if (handler == nullptr) {
    return;
  }
  *handler = std::make_shared<slop::OAuthHandler>(http_client, slop::OAuthHandler::Provider::kOpenAi);
  (*handler)->SetEnabled(true);
}

absl::Status RunSlopRpcService(const ServerRuntimeConfig& server_config) {
  slop::Database db;
  RETURN_IF_ERROR(db.Init(server_config.db_path));
  slop::HttpClient http_client;
  ASSIGN_OR_RETURN(slop::RuntimeBootstrap runtime,
                   slop::BootstrapRuntime(
                       &db, &http_client, server_config.runtime_options,
                       [&](std::shared_ptr<slop::OAuthHandler>* handler) {
                         ConfigureRpcOpenAiOAuthHandler(&http_client, handler);
                       },
                       [&](slop::ToolExecutor&) -> std::vector<std::string> { return server_config.active_skills; },
                       [](slop::CommandHandler&) {}));
  ApplyServerExecutionPolicy(*runtime.tool_executor, server_config);

  SlopServiceImpl service(server_config, std::move(runtime), &db);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_config.listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (server == nullptr) {
    return absl::InternalError("failed to start slop rpc service");
  }
  server->Wait();
  return absl::OkStatus();
}

}  // namespace slop::rpc::v1
