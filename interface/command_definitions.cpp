#include "interface/command_definitions.h"
namespace slop {
const std::vector<CommandDefinition>& GetCommandDefinitions() {
  static const std::vector<CommandDefinition> kDefinitions = {
      {"/help", {}, {}, {"Show this help message"}, "Core Operations"},
      {"/exit", {}, {"/quit"}, {"Exit the program"}, "Core Operations"},
      {"/edit", {}, {}, {"Open last input in EDITOR"}, "Core Operations"},
      {"/exec", {}, {}, {"/exec <command>        Execute shell command"}, "Core Operations"},
      {"/stats", {}, {"/usage"}, {"Show session usage statistics"}, "Core Operations"},
      {"/session", {"list", "switch", "remove", "clear", "clone", "scratchpad"},
      {"/agents_md", {"show", "reload"}, {"/memo"}, {"/agents_md show", "/agents_md reload [path]"}, "Session & Memory"}, {}, {"/session list", "/session switch <name>", "/session remove <name>", "/session clear", "/session clone <name>", "/session scratchpad read", "/session scratchpad edit"}, "Session & Memory"},
      {"/message", {"list", "show", "remove"}, {"/messages"}, {"/message list [N]", "/message show <GID>", "/message remove <GID>"}, "Context & History"},
      {"/undo", {}, {}, {"Remove last message"}, "Context & History"},
      {"/context", {"show", "window", "rebuild"}, {}, {"/context show", "/context window <N>", "/context rebuild"}, "Context & History"},
      {"/review", {"session", "mail", "mail approve", "git"}, {}, {"/review session", "/review mail", "/review mail approve", "/review git <ref>"}, "Context & History"},
      {"/feedback", {}, {}, {"/feedback"}, "Context & History"},
      {"/model", {}, {}, {"/model <name>"}, "Model & Configuration"},
      {"/models", {}, {}, {"/models [filter]"}, "Model & Configuration"},
      {"/throttle", {}, {}, {"/throttle [N]"}, "Model & Configuration"},
      {"/schema", {}, {}, {"Show schema"}, "Model & Configuration"},
      {"/mode", {"mail", "standard"}, {}, {"/mode [mail|standard]"}, "Model & Configuration"},
      {"/tool", {"list", "show"}, {}, {"/tool list", "/tool show <name>"}, "Agent Capabilities"},
      {"/skill", {"list", "activate", "deactivate", "add", "edit", "delete"}, {}, {"/skill list", "/skill activate <ID>", "/skill deactivate <ID>", "/skill add", "/skill edit <ID>", "/skill delete <ID>", "hey <skill> <query>"}, "Agent Capabilities"},
  }; return kDefinitions; } } // namespace slop
