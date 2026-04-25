#ifndef SLOP_APP_RUNTIME_BOOTSTRAP_H_
#define SLOP_APP_RUNTIME_BOOTSTRAP_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "app/llm_tool_specializations.h"
#include "core/config.h"
#include "interface/interaction_engine.h"

namespace slop {

class CancellationRequest;
class CommandHandler;
class Database;
class HttpClient;
class OAuthHandler;
class Orchestrator;
class ToolExecutor;

struct RuntimeBootstrapOptions {
  bool openai_oauth = false;
  bool use_responses = false;
  std::string model;
  std::string google_api_key;
  std::string openai_api_key;
  std::string openai_base_url;
  std::vector<LlmToolSpecializationConfig> llm_specializations;
};

struct RuntimeBootstrap {
  std::shared_ptr<OAuthHandler> oauth_handler;
  std::unique_ptr<Orchestrator> orchestrator;
  std::unique_ptr<ToolExecutor> tool_executor;
  std::unique_ptr<CommandHandler> command_handler;
  std::unique_ptr<InteractionEngine> engine;
  std::unique_ptr<InteractionEngine::Config> engine_config;
};

absl::StatusOr<RuntimeBootstrap> BootstrapRuntime(
    Database* db, HttpClient* http_client, const RuntimeBootstrapOptions& options,
    const std::function<void(std::shared_ptr<OAuthHandler>*)>& configure_openai_oauth_handler,
    const std::function<std::vector<std::string>(ToolExecutor&)>& get_active_skills,
    const std::function<void(CommandHandler&)>& on_command_handler_ready);

}  // namespace slop

#endif  // SLOP_APP_RUNTIME_BOOTSTRAP_H_
