#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/initialize.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "app/llm_tool_specializations.h"
#include "app/mcp_commands.h"
#include "app/prompt_mode.h"
#include "core/config.h"
#include "core/constants.h"
#include "core/database.h"
#include "core/http_client.h"
#include "core/json_utils.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "interface/command_handler.h"
#include "interface/interaction_engine.h"
#include "mcp/runtime.h"
#include "nlohmann/json.hpp"
#include "tools/tool_dispatcher.h"
#include "tools/tool_executor.h"

ABSL_FLAG(std::string, config, "", "Path to the configuration INI file");
ABSL_FLAG(std::string, db, "", "Path to SQLite database (default: slop.db)");
ABSL_FLAG(bool, ephemeral, false, "Use an in-memory database");
ABSL_FLAG(std::string, model, "", "Model name");
ABSL_FLAG(std::string, openai_api_key, "", "OpenAI-compatible API key");
ABSL_FLAG(std::string, openai_base_url, "", "OpenAI-compatible API base URL");
ABSL_FLAG(bool, openai_oauth, false, "Use the OpenAI OAuth token");
ABSL_FLAG(std::string, openai_oauth_token_path, "", "Override the OpenAI OAuth token path");
ABSL_FLAG(std::string, session, "", "Session name (default: default_session)");
ABSL_FLAG(std::string, prompt, "", "Run one prompt and exit");
ABSL_FLAG(std::string, prompt_file, "", "Read one prompt from a file and exit");
ABSL_FLAG(bool, json, false, "Emit JSON output");
ABSL_FLAG(std::string, schema, "", "JSON Schema file for structured prompt output");
ABSL_FLAG(int, limit, 10, "Maximum number of rows for list commands");
ABSL_FLAG(int, retain_groups, 2, "Number of completed context groups to retain");
ABSL_FLAG(int, watermark_tokens, 350000, "Context reset watermark in tokens");
ABSL_FLAG(std::string, file, "", "Input file for a subcommand");

namespace {

using slop::Database;
using slop::json_dump;
using slop::json_get_or;
using slop::json_parse;

int PrintError(const absl::Status& status, bool json, const std::string& command = "") {
  if (!json) {
    std::cerr << "Error: " << status.message() << std::endl;
    return 1;
  }
  nlohmann::json output = { {"ok", false}, {"command", command},
                            {"error", {{"code", absl::StatusCodeToString(status.code())},
                                        {"message", std::string(status.message())}}} };
  std::cout << json_dump(output) << std::endl;
  return 1;
}

int PrintSuccess(const std::string& command, const nlohmann::json& result, bool json, const std::string& text) {
  if (json) {
    nlohmann::json output = { {"ok", true}, {"command", command}, {"result", result} };
    std::cout << json_dump(output) << std::endl;
  } else {
    std::cout << text << std::endl;
  }
  return 0;
}

absl::StatusOr<nlohmann::json> QueryJson(Database* db, const std::string& sql,
                                         const std::vector<std::string>& params = {}) {
  auto result_or = db->Query(sql, params);
  if (!result_or.ok()) return result_or.status();
  auto result = json_parse(*result_or);
  if (!result.has_value()) return absl::InternalError("Database returned invalid JSON");
  return *result;
}

std::string DefaultSession() {
  const std::string session = absl::GetFlag(FLAGS_session);
  return session.empty() ? "default_session" : session;
}

absl::Status ValidateCommonFlags() {
  if (absl::GetFlag(FLAGS_ephemeral) && !absl::GetFlag(FLAGS_db).empty()) {
    return absl::InvalidArgumentError("--ephemeral and --db are mutually exclusive");
  }
  if (!absl::GetFlag(FLAGS_schema).empty() && !absl::GetFlag(FLAGS_json)) {
    return absl::InvalidArgumentError("--schema requires --json");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Database>> OpenDatabase() {
  auto db = std::make_unique<Database>();
  const std::string path = absl::GetFlag(FLAGS_ephemeral)
                               ? ":memory:"
                               : (absl::GetFlag(FLAGS_db).empty() ? "slop.db" : absl::GetFlag(FLAGS_db));
  auto status = db->Init(path);
  if (!status.ok()) return status;
  return db;
}

int RunContext(Database* db, const std::vector<std::string>& args, bool json) {
  const std::string session = DefaultSession();
  if (args.empty() || (args.size() == 1 && args[0] == "show")) {
    auto settings_or = db->GetAccordionContextSettings(session);
    if (!settings_or.ok()) return PrintError(settings_or.status(), json, "context show");
    auto prompt_tokens_or = db->GetLatestPromptTokens(session);
    nlohmann::json result = { {"session", session},
                              {"retain_groups", settings_or->retain_groups},
                              {"watermark_tokens", settings_or->watermark_tokens},
                              {"epoch_start_group", settings_or->epoch_start_group_id},
                              {"latest_prompt_tokens", prompt_tokens_or.ok() && prompt_tokens_or->has_value()
                                                             ? nlohmann::json(**prompt_tokens_or)
                                                             : nlohmann::json(nullptr)} };
    const std::string text = absl::StrCat("Session: ", session, "\nRetain groups: ", settings_or->retain_groups,
                                          "\nWatermark tokens: ", settings_or->watermark_tokens,
                                          "\nEpoch start group: ",
                                          settings_or->epoch_start_group_id.empty() ? "not initialized"
                                                                                     : settings_or->epoch_start_group_id);
    return PrintSuccess("context show", result, json, text);
  }
  if (args.size() != 1 || args[0] != "set") {
    return PrintError(absl::InvalidArgumentError(
                          "usage: sl context show|set [--retain_groups N --watermark_tokens N]"),
                      json, "context");
  }
  auto status = db->SetAccordionContextSettings(session, absl::GetFlag(FLAGS_retain_groups),
                                                absl::GetFlag(FLAGS_watermark_tokens));
  if (!status.ok()) return PrintError(status, json, "context set");
  nlohmann::json result = { {"session", session},
                            {"retain_groups", absl::GetFlag(FLAGS_retain_groups)},
                            {"watermark_tokens", absl::GetFlag(FLAGS_watermark_tokens)} };
  return PrintSuccess("context set", result, json,
                      absl::StrCat("Context settings saved for ", session));
}

int RunSession(Database* db, const std::vector<std::string>& args, bool json) {
  if (args.empty() || (args.size() == 1 && args[0] == "list")) {
    auto rows_or = QueryJson(db, "SELECT DISTINCT session_id FROM messages UNION SELECT DISTINCT id FROM sessions");
    if (!rows_or.ok()) return PrintError(rows_or.status(), json, "session list");
    std::string text;
    for (const auto& row : *rows_or) {
      text += absl::StrCat(json_get_or(row, "session_id", json_get_or(row, "id", std::string{})), "\n");
    }
    return PrintSuccess("session list", *rows_or, json, text);
  }
  if (args[0] == "remove" && args.size() == 2) {
    auto status = db->DeleteSession(args[1]);
    if (!status.ok()) return PrintError(status, json, "session remove");
    return PrintSuccess("session remove", {{"session", args[1]}}, json, absl::StrCat("Removed ", args[1]));
  }
  if (args[0] == "clone" && args.size() == 2) {
    auto status = db->CloneSession(DefaultSession(), args[1]);
    if (!status.ok()) return PrintError(status, json, "session clone");
    return PrintSuccess("session clone", {{"source", DefaultSession()}, {"target", args[1]}}, json,
                        absl::StrCat("Cloned ", DefaultSession(), " to ", args[1]));
  }
  if (args[0] == "rollback" && args.size() == 2) {
    auto status = db->RollbackSessionToGroup(DefaultSession(), args[1]);
    if (!status.ok()) return PrintError(status, json, "session rollback");
    return PrintSuccess("session rollback", {{"session", DefaultSession()}, {"group", args[1]}}, json,
                        absl::StrCat("Rolled back ", DefaultSession(), " to ", args[1]));
  }
  return PrintError(absl::InvalidArgumentError(
                        "usage: sl session list|remove NAME|clone NAME|rollback GROUP"),
                    json, "session");
}

int RunMessage(Database* db, const std::vector<std::string>& args, bool json) {
  if (args.empty() || (args.size() == 1 && args[0] == "list")) {
    const int limit = absl::GetFlag(FLAGS_limit);
    if (limit < 1) return PrintError(absl::InvalidArgumentError("--limit must be positive"), json, "message list");
    auto rows_or = QueryJson(
        db, "SELECT m1.group_id, m1.content AS prompt, MAX(m2.tokens) AS tokens FROM messages m1 "
            "LEFT JOIN messages m2 ON m1.group_id = m2.group_id AND m2.role = 'assistant' "
            "WHERE m1.session_id = ? AND m1.role = 'user' GROUP BY m1.group_id "
            "ORDER BY m1.created_at DESC LIMIT ?",
        {DefaultSession(), std::to_string(limit)});
    if (!rows_or.ok()) return PrintError(rows_or.status(), json, "message list");
    std::string text;
    for (const auto& row : *rows_or) {
      std::string prompt = json_get_or(row, "prompt", std::string{});
      if (prompt.size() > 80) prompt = prompt.substr(0, 77) + "...";
      text += absl::StrCat(json_get_or(row, "group_id", std::string{}), "\t", prompt, "\n");
    }
    return PrintSuccess("message list", *rows_or, json, text);
  }
  if ((args[0] == "show" || args[0] == "view") && args.size() == 2) {
    auto rows_or = QueryJson(db, "SELECT role, content, tokens FROM messages WHERE group_id = ? ORDER BY created_at ASC",
                             {args[1]});
    if (!rows_or.ok()) return PrintError(rows_or.status(), json, "message show");
    std::string text;
    for (const auto& row : *rows_or) {
      text += absl::StrCat("[", json_get_or(row, "role", std::string{"unknown"}), "]\n",
                           json_get_or(row, "content", std::string{}), "\n\n");
    }
    return PrintSuccess("message show", *rows_or, json, text);
  }
  if (args[0] == "remove" && args.size() == 2) {
    auto status = db->Execute("DELETE FROM messages WHERE group_id = ?", {args[1]});
    if (!status.ok()) return PrintError(status, json, "message remove");
    return PrintSuccess("message remove", {{"group", args[1]}}, json, absl::StrCat("Removed ", args[1]));
  }
  return PrintError(absl::InvalidArgumentError("usage: sl message list|show GROUP|remove GROUP"), json, "message");
}

int RunScratchpad(Database* db, const std::vector<std::string>& args, bool json) {
  const std::string session = DefaultSession();
  if (args.empty() || (args.size() == 1 && args[0] == "show")) {
    auto content_or = db->GetScratchpad(session);
    if (!content_or.ok()) return PrintError(content_or.status(), json, "scratchpad show");
    return PrintSuccess("scratchpad show", {{"session", session}, {"content", *content_or}}, json, *content_or);
  }
  if (args[0] == "save" && args.size() == 1) {
    auto content_or = db->GetLastAssistantMessage(session);
    if (!content_or.ok()) return PrintError(content_or.status(), json, "scratchpad save");
    auto status = db->SetScratchpad(session, *content_or);
    if (!status.ok()) return PrintError(status, json, "scratchpad save");
    return PrintSuccess("scratchpad save", {{"session", session}}, json, "Saved last assistant message.");
  }
  if (args[0] == "set" && absl::GetFlag(FLAGS_file).empty()) {
    return PrintError(absl::InvalidArgumentError("scratchpad set requires --file"), json, "scratchpad set");
  }
  if (args[0] == "set" && args.size() == 1) {
    std::ifstream file(absl::GetFlag(FLAGS_file));
    if (!file.is_open()) return PrintError(absl::NotFoundError("Failed to open scratchpad file"), json, "scratchpad set");
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto status = db->SetScratchpad(session, content);
    if (!status.ok()) return PrintError(status, json, "scratchpad set");
    return PrintSuccess("scratchpad set", {{"session", session}}, json, "Scratchpad saved.");
  }
  return PrintError(absl::InvalidArgumentError("usage: sl scratchpad show|save|set --file PATH"), json, "scratchpad");
}

int RunSkill(Database* db, const std::vector<std::string>& args, bool json) {
  const std::string session = DefaultSession();
  if (args.empty() || (args.size() == 1 && args[0] == "list")) {
    auto skills_or = db->GetSkills();
    if (!skills_or.ok()) return PrintError(skills_or.status(), json, "skill list");
    nlohmann::json result = nlohmann::json::array();
    std::string text;
    for (const auto& skill : *skills_or) {
      result.push_back({ {"id", skill.id}, {"name", skill.name}, {"description", skill.description},
                         {"activation_count", skill.activation_count} });
      text += absl::StrCat(skill.name, "\t", skill.description, "\n");
    }
    return PrintSuccess("skill list", result, json, text);
  }
  if ((args[0] == "activate" || args[0] == "deactivate") && args.size() == 2) {
    auto exists_or = db->SkillExists(args[1]);
    if (!exists_or.ok()) return PrintError(exists_or.status(), json, absl::StrCat("skill ", args[0]));
    if (!*exists_or) return PrintError(absl::NotFoundError(absl::StrCat("Skill not found: ", args[1])), json,
                                       absl::StrCat("skill ", args[0]));
    auto active_or = db->GetActiveSkills(session);
    if (!active_or.ok()) return PrintError(active_or.status(), json, absl::StrCat("skill ", args[0]));
    std::vector<std::string> active = *active_or;
    auto it = std::find(active.begin(), active.end(), args[1]);
    if (args[0] == "activate" && it == active.end()) active.push_back(args[1]);
    if (args[0] == "deactivate" && it != active.end()) active.erase(it);
    auto status = db->SetActiveSkills(session, active);
    if (!status.ok()) return PrintError(status, json, absl::StrCat("skill ", args[0]));
    return PrintSuccess(absl::StrCat("skill ", args[0]), {{"session", session}, {"skill", args[1]}}, json,
                        absl::StrCat("Updated skills for ", session));
  }
  return PrintError(absl::InvalidArgumentError("usage: sl skill list|activate NAME|deactivate NAME"), json, "skill");
}

int RunModel(const std::vector<std::string>& args, bool json) {
  if (!args.empty() && !(args.size() == 1 && args[0] == "show")) {
    return PrintError(absl::InvalidArgumentError("usage: sl model show"), json, "model");
  }
  const std::string model = absl::GetFlag(FLAGS_model).empty() ? "gpt-5.4-mini:high" : absl::GetFlag(FLAGS_model);
  return PrintSuccess("model show", {{"model", model}}, json, absl::StrCat("Current model: ", model));
}

int RunStats(Database* db, const std::vector<std::string>& args, bool json) {
  if (!args.empty()) {
    return PrintError(absl::InvalidArgumentError("usage: sl stats"), json, "stats");
  }
  auto usage_or = db->GetTotalUsage(DefaultSession());
  if (!usage_or.ok()) return PrintError(usage_or.status(), json, "stats");
  const auto& usage = *usage_or;
  nlohmann::json result = { {"session", DefaultSession()},
                            {"prompt_tokens", usage.prompt_tokens},
                            {"completion_tokens", usage.completion_tokens},
                            {"total_tokens", usage.total_tokens},
                            {"cached_prompt_tokens", usage.cached_prompt_tokens},
                            {"cache_write_prompt_tokens", usage.cache_write_prompt_tokens} };
  return PrintSuccess("stats", result, json,
                      absl::StrCat("Prompt tokens: ", usage.prompt_tokens, "\nCompletion tokens: ",
                                   usage.completion_tokens, "\nTotal tokens: ", usage.total_tokens));
}

int RunTool(Database* db, const std::vector<std::string>& args, bool json) {
  if (args.empty() || (args.size() == 1 && args[0] == "list")) {
    auto tools_or = db->GetTopLevelTools();
    if (!tools_or.ok()) return PrintError(tools_or.status(), json, "tool list");
    nlohmann::json result = nlohmann::json::array();
    std::string text;
    for (const auto& tool : *tools_or) {
      result.push_back({ {"name", tool.name}, {"description", tool.description}, {"enabled", tool.is_enabled},
                         {"call_count", tool.call_count} });
      text += absl::StrCat(tool.name, "\t", tool.description, "\n");
    }
    return PrintSuccess("tool list", result, json, text);
  }
  if (args.size() != 2 || args[0] != "show") {
    return PrintError(absl::InvalidArgumentError("usage: sl tool list|show NAME"), json, "tool");
  }
  auto tools_or = db->GetTopLevelTools();
  if (!tools_or.ok()) return PrintError(tools_or.status(), json, "tool list");
  nlohmann::json result = nlohmann::json::array();
  std::string text;
  for (const auto& tool : *tools_or) {
    if (!args.empty() && args[0] == "show" && (args.size() != 2 || tool.name != args[1])) continue;
    result.push_back({ {"name", tool.name}, {"description", tool.description}, {"enabled", tool.is_enabled},
                       {"call_count", tool.call_count} });
    text += absl::StrCat(tool.name, "\t", tool.description, "\n");
  }
  if (!args.empty() && args[0] == "show" && result.empty()) {
    return PrintError(absl::NotFoundError("Tool not found"), json, "tool show");
  }
  return PrintSuccess(args.empty() || args[0] == "list" ? "tool list" : "tool show", result, json, text);
}

int RunSubcommand(Database* db, const std::vector<std::string>& positional, bool json) {
  if (positional.empty()) {
    return PrintError(absl::InvalidArgumentError(
                          "usage: sl <context|session|message|scratchpad|skill|model|stats|tool> <command> [args]"),
                      json, "sl");
  }
  const std::string command = positional[0];
  std::vector<std::string> args(positional.begin() + 1, positional.end());
  if (command == "context") return RunContext(db, args, json);
  if (command == "session") return RunSession(db, args, json);
  if (command == "message") return RunMessage(db, args, json);
  if (command == "scratchpad") return RunScratchpad(db, args, json);
  if (command == "skill") return RunSkill(db, args, json);
  if (command == "model") return RunModel(args, json);
  if (command == "stats") return RunStats(db, args, json);
  if (command == "tool") return RunTool(db, args, json);
  return PrintError(absl::NotFoundError(absl::StrCat("Unknown command: ", command)), json, command);
}

std::string FlagName(const std::string& argument) {
  std::string normalized = argument;
  if (normalized.size() > 1 && normalized[0] == '-' && normalized[1] != '-') normalized.insert(0, "-");
  const size_t equals = normalized.find('=');
  return equals == std::string::npos ? normalized : normalized.substr(0, equals);
}

bool IsFlagArgument(const std::string& argument) {
  return argument.size() > 1 && argument[0] == '-';
}

bool IsGlobalValueFlag(const std::string& argument) {
  return argument == "--config" || argument == "--db" || argument == "--model" ||
         argument == "--openai_api_key" || argument == "--openai_base_url" ||
         argument == "--openai_oauth_token_path" || argument == "--session" || argument == "--prompt" ||
         argument == "--prompt_file" || argument == "--schema" || argument == "--limit" ||
         argument == "--retain_groups" || argument == "--watermark_tokens" || argument == "--file" ||
         argument == "--flagfile" || argument == "--undefok" || argument == "--fromenv" ||
         argument == "--tryfromenv";
}

bool IsKnownGlobalFlag(const std::string& argument) {
  const std::string name = FlagName(argument);
  return name == "--config" || name == "--db" || name == "--ephemeral" || name == "--model" ||
         name == "--openai_api_key" || name == "--openai_base_url" || name == "--openai_oauth" ||
         name == "--openai_oauth_token_path" || name == "--session" || name == "--prompt" ||
         name == "--prompt_file" || name == "--json" || name == "--schema" || name == "--limit" ||
         name == "--retain_groups" || name == "--watermark_tokens" || name == "--file" ||
         name == "--help" || name == "--h" || name == "--helpshort" || name == "--helpfull" ||
         name == "--helpmatch" || name == "--helpon" || name == "--helppackage" || name == "--helpxml" ||
         name == "--flagfile" || name == "--undefok" || name == "--fromenv" || name == "--tryfromenv";
}

bool IsHelpFlag(const std::string& argument) {
  const std::string name = FlagName(argument);
  return name == "--help" || name == "--h" || name == "--helpshort" || name == "--helpfull" ||
         name == "--helpmatch" || name == "--helpon" || name == "--helppackage" || name == "--helpxml";
}

bool IsGlobalBoolFlag(const std::string& name) {
  return name == "--ephemeral" || name == "--openai_oauth" || name == "--json";
}

bool IsGlobalIntegerFlag(const std::string& name) {
  return name == "--limit" || name == "--retain_groups" || name == "--watermark_tokens";
}

std::optional<bool> BoolFlagValue(const std::string& argument, const std::string& name) {
  if (FlagName(argument) != name) return std::nullopt;
  if (argument.find('=') == std::string::npos) return true;
  const size_t equals = argument.find('=');
  std::string value = argument.substr(equals + 1);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return std::nullopt;
}

std::optional<bool> JsonFlagValue(const std::string& argument) {
  return BoolFlagValue(argument, "--json");
}

bool HasExplicitJsonFlag(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (FlagName(argv[i]) != "--json") continue;
    auto value = JsonFlagValue(argv[i]);
    if (!value.has_value() || *value) return true;
  }
  return false;
}

absl::Status ValidateRawArguments(int argc, char* argv[], int mcp_index) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--") break;
    const bool after_mcp = mcp_index >= 0 && i > mcp_index;
    if (after_mcp && IsFlagArgument(argument) && !IsKnownGlobalFlag(argument)) continue;
    if (!IsFlagArgument(argument)) continue;
    if (!IsKnownGlobalFlag(argument)) {
      return absl::InvalidArgumentError(absl::StrCat("Unknown flag: ", argument));
    }
    const std::string flag_name = FlagName(argument);
    if (IsGlobalBoolFlag(flag_name) && argument.find('=') != std::string::npos &&
        !BoolFlagValue(argument, flag_name).has_value()) {
      return absl::InvalidArgumentError(absl::StrCat("Invalid boolean value: ", argument));
    }
    if (IsGlobalIntegerFlag(flag_name) && argument.find('=') != std::string::npos) {
      int value = 0;
      if (!absl::SimpleAtoi(argument.substr(argument.find('=') + 1), &value)) {
        return absl::InvalidArgumentError(absl::StrCat("Invalid integer value: ", argument));
      }
    }
    if (IsGlobalValueFlag(flag_name) && argument.find('=') == std::string::npos) {
      if (i + 1 >= argc) {
        return absl::InvalidArgumentError(absl::StrCat("Flag requires a value: ", argument));
      }
      const std::string value = argv[i + 1];
      if (IsGlobalIntegerFlag(flag_name)) {
        int parsed = 0;
        if (!absl::SimpleAtoi(value, &parsed)) {
          return absl::InvalidArgumentError(absl::StrCat("Invalid integer value: ", argument, " ", value));
        }
      } else if (IsFlagArgument(value)) {
        return absl::InvalidArgumentError(absl::StrCat("Flag requires a value: ", argument));
      }
      ++i;
    }
  }
  return absl::OkStatus();
}

int FindMcpCommand(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--") return -1;
    if (!IsFlagArgument(argument)) return argument == "mcp" ? i : -1;
    if (!IsKnownGlobalFlag(argument)) return -1;
    if (IsGlobalValueFlag(FlagName(argument)) && argument.find('=') == std::string::npos) ++i;
  }
  return -1;
}

int RunMcp(const std::vector<std::string>& args, bool json) {
  for (size_t i = 1; i < args.size(); ++i) {
    if (!IsHelpFlag(args[i])) continue;
    slop::HttpClient help_http_client;
    std::ostringstream help_output;
    std::ostringstream help_errors;
    const absl::Status help_status =
        slop::RunMcpCommand({"mcp"}, &help_http_client, &std::cin, &help_output, &help_errors);
    const std::string help_text = std::string(help_status.message());
    return PrintSuccess("mcp", {{"help", help_text}}, json, help_text);
  }

  slop::HttpClient http_client;
  std::ostringstream output;
  std::ostringstream errors;
  const absl::Status status = slop::RunMcpCommand(args, &http_client, &std::cin, &output, &errors);
  if (!status.ok()) {
    if (!errors.str().empty()) output << errors.str();
    return PrintError(status, json, "mcp");
  }
  if (!json) {
    std::cout << output.str();
    return 0;
  }
  return PrintSuccess("mcp", {{"output", output.str()}}, true, "");
}

int PrintPromptError(const absl::Status& status, bool json) {
  if (!json) return PrintError(status, false, "prompt");
  slop::InteractionEngine::PromptRunResult result;
  result.ok = false;
  result.session_id = DefaultSession();
  result.error_code = absl::StatusCodeToString(status.code());
  result.error_message = std::string(status.message());
  std::cout << slop::PromptRunResultToJson(result) << std::endl;
  return 1;
}

int RunPrompt() {
  const bool json = absl::GetFlag(FLAGS_json);
  slop::PromptInputFlags prompt_flags{absl::GetFlag(FLAGS_prompt), absl::GetFlag(FLAGS_prompt_file)};
  if (!slop::HasPromptInputSource(prompt_flags)) {
    return PrintPromptError(absl::InvalidArgumentError("sl requires --prompt or --prompt-file"), json);
  }
  std::optional<nlohmann::json> schema;
  if (!absl::GetFlag(FLAGS_schema).empty()) {
    slop::StructuredFormatFlags format_flags{"", absl::GetFlag(FLAGS_schema)};
    auto schema_or = slop::ResolveStructuredOutputSchema(format_flags);
    if (!schema_or.ok()) return PrintPromptError(schema_or.status(), json);
    schema = *schema_or;
  }
  auto prompt_or = slop::ResolvePromptInput(prompt_flags, isatty(STDIN_FILENO) ? nullptr : &std::cin);
  if (!prompt_or.ok()) return PrintPromptError(prompt_or.status(), json);

  auto specializations_or = slop::LoadLlmToolSpecializations(absl::GetFlag(FLAGS_config));
  if (!specializations_or.ok()) return PrintPromptError(specializations_or.status(), json);
  auto db_or = OpenDatabase();
  if (!db_or.ok()) return PrintPromptError(db_or.status(), json);
  auto db = std::move(*db_or);
  slop::HttpClient http_client;
  const bool openai_oauth = absl::GetFlag(FLAGS_openai_oauth);
  const std::string openai_key = absl::GetFlag(FLAGS_openai_api_key);
  if (!openai_oauth && openai_key.empty()) {
    return PrintPromptError(absl::UnauthenticatedError("Configure an API key or use --openai_oauth"), json);
  }

  std::shared_ptr<slop::OAuthHandler> oauth_handler;
  if (openai_oauth) {
    oauth_handler = std::make_shared<slop::OAuthHandler>(&http_client, slop::OAuthHandler::Provider::kOpenAi);
    if (!absl::GetFlag(FLAGS_openai_oauth_token_path).empty()) {
      oauth_handler->SetTokenPath(absl::GetFlag(FLAGS_openai_oauth_token_path));
    }
    oauth_handler->SetEnabled(true);
    auto token_or = oauth_handler->GetValidToken();
    if (!token_or.ok()) return PrintPromptError(token_or.status(), json);
  }

  slop::Orchestrator::Builder builder(db.get(), &http_client);
  const std::string base_url = openai_oauth
                                   ? slop::kOpenAiChatGptCodexBaseUrl
                                   : (absl::GetFlag(FLAGS_openai_base_url).empty()
                                          ? slop::kOpenAIBaseUrl
                                          : absl::GetFlag(FLAGS_openai_base_url));
  builder.WithModel(absl::GetFlag(FLAGS_model).empty() ? "gpt-5.4-mini:high" : absl::GetFlag(FLAGS_model))
      .WithBaseUrl(base_url);
  auto orchestrator_or = builder.Build();
  if (!orchestrator_or.ok()) return PrintPromptError(orchestrator_or.status(), json);
  auto orchestrator = std::move(*orchestrator_or);

  auto executor_or = slop::ToolExecutor::Create(db.get());
  if (!executor_or.ok()) return PrintPromptError(executor_or.status(), json);
  auto executor = std::move(*executor_or);
  auto dispatcher = std::make_unique<slop::ToolDispatcher>(
      [&executor](const std::string& name, const nlohmann::json& args,
                  std::shared_ptr<slop::CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
        return executor->Execute(name, args, cancellation);
      });
  executor->SetDispatcher(std::move(dispatcher));
  auto mcp_runtime_or = slop::mcp::StartMcpRuntime(db.get(), executor.get(), &http_client);
  if (!mcp_runtime_or.ok()) return PrintPromptError(mcp_runtime_or.status(), json);
  auto command_handler_or = slop::CommandHandler::Create(db.get(), orchestrator.get(), oauth_handler.get(), openai_key);
  if (!command_handler_or.ok()) return PrintPromptError(command_handler_or.status(), json);
  auto command_handler = std::move(*command_handler_or);
  const std::string session = DefaultSession();
  executor->SetSessionId(session);
  const std::vector<std::string> active_skills = executor->GetActiveSkills();
  slop::InteractionEngine engine(*db, *orchestrator, *command_handler, *executor->dispatcher(), *executor, http_client,
                                 oauth_handler);
  slop::InteractionEngine::Config config;
  config.openai_api_key = openai_key;
  config.openai_base_url = base_url;
  config.openai_oauth = openai_oauth;
  config.is_batch_mode = true;
  config.silent = json || schema.has_value();
  config.structured_output_schema = schema;

  auto llm_query_invoker = [&engine, config](const std::string& query, const std::vector<std::string>& skills,
                                             const slop::LlmQueryOptions& options)
      -> absl::StatusOr<std::string> {
    slop::InteractionEngine::QueryOptions query_options;
    query_options.session_id = options.session_id;
    query_options.skill = options.skill;
    query_options.context_window = options.context_window;
    query_options.execution_scope = options.execution_scope == slop::LlmQueryOptions::ExecutionScope::kSubquery
                                        ? slop::InteractionEngine::QueryOptions::ExecutionScope::kSubquery
                                        : slop::InteractionEngine::QueryOptions::ExecutionScope::kRoot;
    query_options.execution_depth = options.execution_depth;
    return engine.Query(query, config, skills, query_options);
  };
  executor->RegisterTool(
      "llm_query",
      [llm_query_invoker, active_skills](const nlohmann::json& args,
                                         std::shared_ptr<slop::CancellationRequest>) -> absl::StatusOr<std::string> {
        auto query = slop::json_get<std::string>(args, "query");
        if (!query) return absl::InvalidArgumentError("Missing 'query' argument");
        slop::LlmQueryOptions options;
        options.session_id = "query";
        options.execution_scope = slop::LlmQueryOptions::ExecutionScope::kRoot;
        options.execution_depth = 0;
        return llm_query_invoker(*query, active_skills, options);
      });

  const auto& specializations = *specializations_or;
  auto register_status = slop::ReconcileLlmSpecializationTools(db.get(), specializations);
  if (!register_status.ok()) return PrintPromptError(register_status, json);
  register_status =
      slop::RegisterLlmSpecializationHandlers(executor.get(), specializations, active_skills, llm_query_invoker);
  if (!register_status.ok()) return PrintPromptError(register_status, json);

  const auto result = engine.ProcessPrompt(*prompt_or, session, active_skills, config);
  if (schema.has_value() && result.ok && result.structured_output.has_value()) {
    std::cout << slop::json_dump(*result.structured_output) << std::endl;
  } else if (json) {
    std::cout << slop::PromptRunResultToJson(result) << std::endl;
  } else if (result.ok) {
    std::cout << result.assistant_message << std::endl;
  } else {
    std::cerr << "Error: " << result.error_message << std::endl;
  }
  return result.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::InitializeSymbolizer(argv[0]);
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions{});
  absl::InitializeLog();
  absl::SetProgramUsageMessage(
      "sl [--prompt TEXT|--prompt-file FILE] [--json] [--schema FILE] [--db PATH|--ephemeral]\n"
      "sl context|session|message|scratchpad|skill|model|stats|tool ...\n"
      "sl mcp ...");

  const bool raw_json = HasExplicitJsonFlag(argc, argv);
  const int mcp_index = FindMcpCommand(argc, argv);
  if (auto status = ValidateRawArguments(argc, argv, mcp_index); !status.ok()) {
    return PrintError(status, raw_json, "sl");
  }
  if (mcp_index >= 0) {
    bool json = false;
    std::vector<std::string> mcp_args{"mcp"};
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      auto json_value = JsonFlagValue(argument);
      if (json_value.has_value()) {
        json = *json_value;
        continue;
      }
      if (i <= mcp_index) continue;
      const std::string flag_name = FlagName(argument);
      if (IsKnownGlobalFlag(argument) && !IsHelpFlag(argument)) {
        if (IsGlobalValueFlag(flag_name) && argument.find('=') == std::string::npos) ++i;
        continue;
      }
      mcp_args.push_back(argument);
    }
    return RunMcp(mcp_args, json);
  }
  const std::vector<char*> positional_args = absl::ParseCommandLine(argc, argv);
  const bool json_on_cli = absl::GetFlag(FLAGS_json);
  if (auto status = ValidateCommonFlags(); !status.ok()) return PrintError(status, json_on_cli, "sl");
  slop::LoadConfigAndApply(absl::GetFlag(FLAGS_config));
  if (absl::GetFlag(FLAGS_json) && !json_on_cli) {
    return PrintError(absl::InvalidArgumentError("JSON output requires an explicit --json flag"), false, "sl");
  }
  if (auto status = ValidateCommonFlags(); !status.ok()) return PrintError(status, json_on_cli, "sl");

  std::vector<std::string> positional;
  for (size_t i = 1; i < positional_args.size(); ++i) positional.emplace_back(positional_args[i]);
  if (!positional.empty()) {
    const bool json = absl::GetFlag(FLAGS_json);
    const std::string& command = positional[0];
    const std::vector<std::string> args(positional.begin() + 1, positional.end());
    if (command == "model") return RunModel(args, json);
    if (command != "context" && command != "session" && command != "message" && command != "scratchpad" &&
        command != "skill" && command != "stats" && command != "tool") {
      return PrintError(absl::NotFoundError(absl::StrCat("Unknown command: ", command)), json, command);
    }
    auto db_or = OpenDatabase();
    if (!db_or.ok()) return PrintError(db_or.status(), json, command);
    auto db = std::move(*db_or);
    return RunSubcommand(db.get(), positional, json);
  }
  return RunPrompt();
}
