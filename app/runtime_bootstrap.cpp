#include "app/runtime_bootstrap.h"

#include <iostream>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "app/llm_tool_specializations.h"
#include "core/constants.h"
#include "core/database.h"
#include "core/http_client.h"
#include "core/json_utils.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "interface/command_handler.h"
#include "tools/tool_dispatcher.h"
#include "tools/tool_executor.h"

namespace slop {

absl::StatusOr<RuntimeBootstrap> BootstrapRuntime(
    Database* db, HttpClient* http_client, const RuntimeBootstrapOptions& options,
    const std::function<void(std::shared_ptr<OAuthHandler>*)>& configure_openai_oauth_handler,
    const std::function<std::vector<std::string>(ToolExecutor&)>& get_active_skills,
    const std::function<void(CommandHandler&)>& on_command_handler_ready) {
  if (db == nullptr) {
    return absl::InvalidArgumentError("Database cannot be null");
  }
  if (http_client == nullptr) {
    return absl::InvalidArgumentError("HttpClient cannot be null");
  }
  if (!configure_openai_oauth_handler) {
    return absl::InvalidArgumentError("configure_openai_oauth_handler must not be empty");
  }
  if (!get_active_skills) {
    return absl::InvalidArgumentError("get_active_skills must not be empty");
  }
  if (!options.openai_oauth && options.google_api_key.empty() && options.openai_api_key.empty()) {
    return absl::InvalidArgumentError("No authentication method found. Configure at least one authentication method.");
  }

  RuntimeBootstrap runtime;

  Orchestrator::Builder builder(db, http_client);
  std::string resolved_openai_base_url = options.openai_base_url;
  if (options.openai_oauth || !options.openai_api_key.empty()) {
    const bool openai_responses = options.openai_oauth || options.use_responses;
    if (options.openai_oauth) {
      resolved_openai_base_url = kOpenAiChatGptCodexBaseUrl;
      if (!options.openai_base_url.empty()) {
        std::cout << "--openai_base_url ignored in --openai_oauth mode; using " << kOpenAiChatGptCodexBaseUrl << "."
                  << std::endl;
      }
    } else {
      resolved_openai_base_url = !options.openai_base_url.empty() ? options.openai_base_url : kOpenAIBaseUrl;
    }

    builder.WithProvider(Orchestrator::Provider::OPENAI)
        .WithModel(!options.model.empty() ? options.model : "gpt-5.4-mini:high")
        .WithBaseUrl(resolved_openai_base_url)
        .WithOpenAiApiStyle(openai_responses ? Orchestrator::OpenAiApiStyle::RESPONSES
                                             : Orchestrator::OpenAiApiStyle::CHAT_COMPLETIONS);
  } else {
    builder.WithProvider(Orchestrator::Provider::GEMINI)
        .WithModel(!options.model.empty() ? options.model : "gemini-3-flash-preview");
  }

  auto orchestrator_or = builder.Build();
  if (!orchestrator_or.ok()) {
    return orchestrator_or.status();
  }
  runtime.orchestrator = std::move(*orchestrator_or);

  if (options.openai_oauth) {
    configure_openai_oauth_handler(&runtime.oauth_handler);
  }

  auto tool_executor_or = ToolExecutor::Create(db);
  if (!tool_executor_or.ok()) {
    return tool_executor_or.status();
  }
  runtime.tool_executor = std::move(*tool_executor_or);

  ToolExecutor* tool_executor = runtime.tool_executor.get();
  auto dispatcher = std::make_unique<ToolDispatcher>([tool_executor](const std::string& name, const nlohmann::json& args,
                                                                     std::shared_ptr<CancellationRequest> cancellation) {
    return tool_executor->Execute(name, args, cancellation);
  });
  runtime.tool_executor->SetDispatcher(std::move(dispatcher));

  auto command_handler_or = CommandHandler::Create(db, runtime.orchestrator.get(), runtime.oauth_handler.get(),
                                                   options.google_api_key, options.openai_api_key);
  if (!command_handler_or.ok()) {
    return command_handler_or.status();
  }
  runtime.command_handler = std::move(*command_handler_or);
  if (on_command_handler_ready) {
    on_command_handler_ready(*runtime.command_handler);
  }

  runtime.engine = std::make_unique<InteractionEngine>(*db, *runtime.orchestrator, *runtime.command_handler,
                                                        *runtime.tool_executor->dispatcher(), *runtime.tool_executor,
                                                         *http_client,
                                                         runtime.oauth_handler);

  runtime.engine_config = std::make_unique<InteractionEngine::Config>();
  runtime.engine_config->google_api_key = options.google_api_key;
  runtime.engine_config->openai_api_key = options.openai_api_key;
  runtime.engine_config->openai_base_url = options.openai_oauth ? kOpenAiChatGptCodexBaseUrl : options.openai_base_url;
  runtime.engine_config->openai_oauth = options.openai_oauth;
  runtime.engine_config->use_responses = options.openai_oauth || options.use_responses;

  InteractionEngine* engine = runtime.engine.get();
  InteractionEngine::Config* engine_config = runtime.engine_config.get();
  auto llm_query_invoker = [engine, engine_config](const std::string& query, const std::vector<std::string>& skills,
                                                   const LlmQueryOptions& options) -> absl::StatusOr<std::string> {
    InteractionEngine::QueryOptions query_options;
    query_options.session_id = options.session_id;
    query_options.skill = options.skill;
    query_options.context_window = options.context_window;
    query_options.execution_scope = options.execution_scope == LlmQueryOptions::ExecutionScope::kSubquery
                                        ? InteractionEngine::QueryOptions::ExecutionScope::kSubquery
                                        : InteractionEngine::QueryOptions::ExecutionScope::kRoot;
    query_options.execution_depth = options.execution_depth;
    return engine->Query(query, *engine_config, skills, query_options);
  };

  const std::vector<std::string> active_skills = get_active_skills(*runtime.tool_executor);
  runtime.tool_executor->RegisterTool(
      "llm_query", [llm_query_invoker, active_skills](const nlohmann::json& args,
                                                       std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
        auto query = json_get<std::string>(args, "query");
        if (!query) {
          return absl::InvalidArgumentError("Missing 'query' argument");
        }

        LlmQueryOptions query_options;
        query_options.session_id = "query";
        query_options.execution_scope = LlmQueryOptions::ExecutionScope::kRoot;
        query_options.execution_depth = 0;
        return llm_query_invoker(*query, active_skills, query_options);
      });

  auto register_status = ReconcileLlmSpecializationTools(db, options.llm_specializations);
  if (!register_status.ok()) {
    return register_status;
  }
  register_status = RegisterLlmSpecializationHandlers(runtime.tool_executor.get(), options.llm_specializations,
                                                      active_skills, llm_query_invoker);
  if (!register_status.ok()) {
    return register_status;
  }

  return runtime;
}

}  // namespace slop
