#include "command_definitions.h"

namespace slop {

const std::vector<CommandDefinition>& GetCommandDefinitions() {
  static const std::vector<CommandDefinition> kDefinitions = {
      // Core Operations
      {"/help", {}, {}, {"Show this help message"}, "Core Operations"},
      {"/exit", {}, {"/quit"}, {"Exit the program"}, "Core Operations"},
      {"/edit", {}, {}, {"Open last input in EDITOR"}, "Core Operations"},
      {"/exec", {}, {}, {"/exec <command>        Execute shell command"}, "Core Operations"},
      {"/stats", {}, {"/usage"}, {"Show session usage statistics"}, "Core Operations"},

      // Session & Memory
      {"/session",
       {"list", "activate", "remove", "clear", "scratchpad"},
       {},
       {"/session list          List all unique session names in the DB",
        "/session activate <name>  Switch to or create a new session named <name>",
        "/session remove <name>  Delete a session and all its data",
        "/session clear         Clear all history and state for current session",
        "/session scratchpad read  Read the current scratchpad",
        "/session scratchpad edit  Open the scratchpad in your $EDITOR"},
       "Session & Memory"},
      {"/memo",
       {"list", "add", "search"},
       {},
       {"/memo list            List all memos",
        "/memo add <tags> <content>  Add a new memo (tags as comma-separated list)",
        "/memo search <tags>   Search memos by tags (comma-separated list)"},
       "Session & Memory"},

      // Context & History
      {"/message",
       {"list", "show", "remove"},
       {"/messages"},
       {"/message list [N]      List last N messages", "/message show <GID>    View full content of a group",
        "/message remove <GID>  Delete a message group"},
       "Context & History"},
      {"/undo", {}, {}, {"Remove last message and rebuild context"}, "Context & History"},
      {"/context",
       {"show", "window", "rebuild"},
       {},
       {"/context show          Show context status and assembled prompt",
        "/context window <N>    Set context to a rolling window of last N groups (0 for full)",
        "/context rebuild       Rebuild session state from conversation history"},
       "Context & History"},
      {"/manual-review",
       {},
       {},
       {"/manual-review         Review session changes (git only). Add comments with 'R:' to instruct the LLM."},
       "Context & History"},

      // Model & Configuration
      {"/model", {}, {}, {"/model <name>          Change active model"}, "Model & Configuration"},
      {"/models", {}, {}, {"/models [filter]       List available models"}, "Model & Configuration"},
      {"/throttle", {}, {}, {"/throttle [N]          Set/show request throttle"}, "Model & Configuration"},
      {"/schema", {}, {}, {"Show current database schema"}, "Model & Configuration"},

      // Agent Capabilities
      {"/tool",
       {"list", "show"},
       {},
       {"/tool list             List available tools", "/tool show <name>      Show tool details"},
       "Agent Capabilities"},
      {"/skill",
       {"list", "activate", "deactivate", "add", "edit", "delete"},
       {},
       {"/skill list            List all available skills", "/skill activate <ID|Name>  Set active skill",
        "/skill deactivate <ID|Name>  Disable active skill", "/skill add             Create new skill",
        "/skill edit <ID|Name>  Modify existing skill", "/skill delete <ID|Name>  Remove skill"},
       "Agent Capabilities"},
  };
  return kDefinitions;
}

}  // namespace slop
