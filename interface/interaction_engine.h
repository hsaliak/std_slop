#ifndef SLOP_INTERFACE_INTERACTION_ENGINE_H_
#define SLOP_INTERFACE_INTERACTION_ENGINE_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cancellation.h"
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
  struct QueryEvent {
    enum class Type {
      kAssistantMessage,
      kToolCall,
      kToolResult,
    };

    Type type;
    std::string id;
    std::string title;
    std::string content;
    std::string status;
  };

  using QueryEventCallback = std::function<void(const QueryEvent& event)>;

  struct Config {
    bool is_batch_mode = false;
    std::string google_api_key;
    std::shared_ptr<CancellationRequest> cancellation;
    std::string openai_api_key;
    std::string openai_base_url;
    bool openai_oauth = false;
    bool use_responses = false;
    bool silent = false;
    QueryEventCallback event_callback;
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
    std::shared_ptr<CancellationRequest> cancellation;
    bool command_mode = false;
  };

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
};

}  // namespace slop

#endif  // SLOP_INTERFACE_INTERACTION_ENGINE_H_
