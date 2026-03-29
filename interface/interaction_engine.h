#ifndef SLOP_INTERFACE_INTERACTION_ENGINE_H_
#define SLOP_INTERFACE_INTERACTION_ENGINE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/database.h"
#include "core/http_client.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "tools/tool_dispatcher.h"
#include "tools/tool_executor.h"
#include "interface/command_handler.h"

namespace slop {

class InteractionEngine {
 public:
  struct Config {
    bool is_batch_mode = false;
    std::string google_api_key;
    std::string openai_api_key;
    std::string openai_base_url;
    bool openai_oauth = false;
    bool use_responses = false;
    bool silent = false;
  };

  InteractionEngine(Database& db, Orchestrator& orchestrator, CommandHandler& cmd_handler, ToolDispatcher& dispatcher,
                    ToolExecutor& tool_executor, HttpClient& http_client, std::shared_ptr<OAuthHandler> oauth_handler);

  // Processes a single user input. Returns true if the interaction loop should continue (standard mode),
  // or false if it should terminate (e.g. exit command).
  bool Process(std::string& input, std::string& session_id, std::vector<std::string>& active_skills,
               const Config& config);

  struct QueryOptions {
    enum class ExecutionScope {
      kRoot,
      kSubquery,
    };

    std::string session_id = "query";
    std::optional<std::string> skill;
    std::optional<int> context_window;
    ExecutionScope execution_scope = ExecutionScope::kRoot;
    int execution_depth = 0;
  };

  absl::StatusOr<std::string> Query(const std::string& prompt, const Config& config,
                                    const std::vector<std::string>& active_skills = {});

  absl::StatusOr<std::string> Query(const std::string& prompt, const Config& config,
                                    const std::vector<std::string>& active_skills,
                                    const QueryOptions& options);

  CommandHandler& GetCommandHandler() { return cmd_handler_; }

 private:
  Database& db_;
  Orchestrator& orchestrator_;
  CommandHandler& cmd_handler_;
  ToolDispatcher& dispatcher_;
  ToolExecutor& tool_executor_;
  HttpClient& http_client_;
  std::shared_ptr<OAuthHandler> oauth_handler_;
};

}  // namespace slop

#endif  // SLOP_INTERFACE_INTERACTION_ENGINE_H_
