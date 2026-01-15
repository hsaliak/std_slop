#ifndef SLOP_SQL_COMMAND_HANDLER_H_
#define SLOP_SQL_COMMAND_HANDLER_H_

#include <string>
#include <vector>
#include <functional>
#include "database.h"
#include "ui.h"

namespace slop {

class CommandHandler {
 public:
  enum class Result {
    HANDLED,       // Command executed, don't send to LLM
    NOT_A_COMMAND, // Not a command, send to LLM
    UNKNOWN,       // Starts with /, but unrecognized. Don't send to LLM.
    PROCEED_TO_LLM // Special case for /edit where we now have LLM input
  };

  explicit CommandHandler(Database* db) : db_(db) {}

  Result Handle(std::string& input, std::string& current_session_id, std::vector<std::string>& active_skills, std::function<void()> show_help_fn, const std::vector<std::string>& selected_groups = {});

 private:
  Database* db_;
};

}  // namespace slop

#endif  // SLOP_SQL_COMMAND_HANDLER_H_
