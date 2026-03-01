#ifndef SLOP_INTERFACE_INTERACTION_ENGINE_H_
#define SLOP_INTERFACE_INTERACTION_ENGINE_H_

#include <memory>
#include <string>
#include <vector>

#include "core/database.h"
#include "core/http_client.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "core/tool_dispatcher.h"
#include "core/tool_executor.h"
#include "interface/command_handler.h"

namespace slop {

class InteractionEngine {
 public:
  struct Config {
    bool is_batch_mode = false;
    std::string google_api_key;
    std::string openai_api_key;
    std::string openai_base_url;
    bool google_oauth = false;
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

  absl::StatusOr<std::string> Query(const std::string& prompt, const Config& config,
                                    const std::vector<std::string>& active_skills = {});

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
