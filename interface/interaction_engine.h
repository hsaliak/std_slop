#ifndef SLOP_INTERFACE_INTERACTION_ENGINE_H_
#define SLOP_INTERFACE_INTERACTION_ENGINE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/database.h"
#include "core/http_client.h"
#include "core/llm_query_options.h"
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
    std::string openai_api_key;
    std::string openai_base_url;
    bool openai_oauth = false;
    bool silent = false;
    std::optional<nlohmann::json> structured_output_schema;
    std::optional<std::string> system_prompt_override;
  };

  InteractionEngine(Database& db, Orchestrator& orchestrator, CommandHandler& cmd_handler, ToolDispatcher& dispatcher,
                    ToolExecutor& tool_executor, HttpClient& http_client, std::shared_ptr<OAuthHandler> oauth_handler);

  // Processes a single user input. Returns true if the interaction loop should continue (standard mode),
  // or false if it should terminate (e.g. exit command).
  bool Process(std::string& input, std::string& session_id, std::vector<std::string>& active_skills,
               const Config& config);

  struct PromptRunResult {
    bool ok = false;
    std::string session_id;
    std::string model;
    std::vector<std::string> active_skills;
    std::string assistant_message;
    std::optional<nlohmann::json> structured_output;
    std::string error_code;
    std::string error_message;
    int64_t duration_ms = 0;
  };

  PromptRunResult ProcessPrompt(std::string input, std::string session_id, std::vector<std::string> active_skills,
                                Config config);

  using QueryOptions = LlmQueryOptions;

  absl::StatusOr<std::string> Query(const std::string& prompt, const Config& config,
                                    const std::vector<std::string>& active_skills = {});

  absl::StatusOr<std::string> Query(const std::string& prompt, const Config& config,
                                    const std::vector<std::string>& active_skills,
                                    const QueryOptions& options);

  // Validates and normalizes untrusted query options before execution.
  // Returns InvalidArgument on malformed values.
  static absl::StatusOr<QueryOptions> NormalizeQueryOptions(const QueryOptions& options);

  static bool IsValidQueryExecutionContext(const QueryOptions& options);

  CommandHandler& GetCommandHandler() { return cmd_handler_; }

 private:
  Database& db_;
  Orchestrator& orchestrator_;
  CommandHandler& cmd_handler_;
  ToolDispatcher& dispatcher_;
  ToolExecutor& tool_executor_;
  HttpClient& http_client_;
  std::shared_ptr<OAuthHandler> oauth_handler_;
  std::optional<nlohmann::json> last_structured_output_;
};

}  // namespace slop

#endif  // SLOP_INTERFACE_INTERACTION_ENGINE_H_
