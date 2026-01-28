#ifndef SLOP_SQL_COMMAND_HANDLER_H_
#define SLOP_SQL_COMMAND_HANDLER_H_

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"

#include "core/database.h"
#include "interface/ui.h"

namespace slop {

class OAuthHandler;

class CommandHandler {
 public:
  enum class Result {
    HANDLED,         // Command executed, don't send to LLM
    NOT_A_COMMAND,   // Not a command, send to LLM
    UNKNOWN,         // Starts with /, but unrecognized. Don't send to LLM.
    PROCEED_TO_LLM,  // Special case for /edit where we now have LLM input
  };

  struct CommandArgs {
    std::string& input;
    std::string& session_id;
    std::vector<std::string>& active_skills;
    std::function<void()> show_help_fn;
    const std::vector<std::string>& selected_groups;
    std::string args;
  };

  using CommandFunc = std::function<Result(CommandArgs&)>;

  virtual ~CommandHandler() = default;

  static absl::StatusOr<std::unique_ptr<CommandHandler>> Create(Database* db,
                                                                class Orchestrator* orchestrator = nullptr,
                                                                OAuthHandler* oauth_handler = nullptr,
                                                                std::string google_api_key = "",
                                                                std::string openai_api_key = "") {
    if (db == nullptr) {
      return absl::InvalidArgumentError("Database cannot be null");
    }
    return std::unique_ptr<CommandHandler>(
        new CommandHandler(db, orchestrator, oauth_handler, std::move(google_api_key), std::move(openai_api_key)));
  }

  Result Handle(std::string& input, std::string& current_session_id, std::vector<std::string>& active_skills,
                std::function<void()> show_help_fn, const std::vector<std::string>& selected_groups = {});

  std::vector<std::string> GetCommandNames() const;
  std::vector<std::string> GetSubCommands(const std::string& command) const;
  const absl::flat_hash_map<std::string, std::vector<std::string>>& GetSubCommandMap() const { return sub_commands_; }

 private:
  void RegisterCommands();

  // Individual command handlers
  Result HandleHelp(CommandArgs& args);
  Result HandleExit(CommandArgs& args);
  Result HandleEdit(CommandArgs& args);
  Result HandleMessage(CommandArgs& args);
  Result HandleUndo(CommandArgs& args);
  Result HandleContext(CommandArgs& args);
  Result HandleTool(CommandArgs& args);
  Result HandleSkill(CommandArgs& args);
  Result HandleSession(CommandArgs& args);
  Result HandleStats(CommandArgs& args);
  Result HandleModels(CommandArgs& args);
  Result HandleExec(CommandArgs& args);
  Result HandleSchema(CommandArgs& args);
  Result HandleModel(CommandArgs& args);
  Result HandleThrottle(CommandArgs& args);
  Result HandleMemo(CommandArgs& args);
  Result HandleManualReview(CommandArgs& args);

  Database* db_;
  class Orchestrator* orchestrator_;
  OAuthHandler* oauth_handler_;
  std::string google_api_key_;
  std::string openai_api_key_;
  absl::flat_hash_map<std::string, CommandFunc> commands_;
  absl::flat_hash_map<std::string, std::vector<std::string>> sub_commands_;

 protected:
  explicit CommandHandler(Database* db, class Orchestrator* orchestrator = nullptr,
                          OAuthHandler* oauth_handler = nullptr, std::string google_api_key = "",
                          std::string openai_api_key = "");

  // Testing hook for dependency injection. Overridden in tests to mock editor input.
  virtual std::string TriggerEditor(const std::string& initial_content);

  // Testing hook for shell commands.
  virtual absl::StatusOr<std::string> ExecuteCommand(const std::string& command);
};

}  // namespace slop

#endif  // SLOP_SQL_COMMAND_HANDLER_H_
