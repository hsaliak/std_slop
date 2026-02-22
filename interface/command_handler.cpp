#include "interface/command_handler.h"
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/substitute.h"
#include "nlohmann/json.hpp"
#include "core/json_utils.h"
#include "core/message_parser.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "core/shell_util.h"
#include "interface/command_definitions.h"
#include "interface/ui.h"
namespace slop {
namespace {
bool HasReviewComments(const std::string& content) {
  std::vector<std::string> lines = absl::StrSplit(content, '\n');
  for (const auto& line : lines) {
    std::string_view trimmed = absl::StripLeadingAsciiWhitespace(line);
    // Skip common list markers
    if (!trimmed.empty() && (trimmed[0] == '*' || trimmed[0] == '-' || trimmed[0] == '>')) {
      trimmed.remove_prefix(1);
      trimmed = absl::StripLeadingAsciiWhitespace(trimmed);
    }
    // Handle numbered lists like "1. R:"
    if (!trimmed.empty() && std::isdigit(trimmed[0])) {
      size_t i = 0;
      while (i < trimmed.size() && std::isdigit(trimmed[i])) i++;
      if (i < trimmed.size() && trimmed[i] == '.') {
        trimmed.remove_prefix(i + 1);
        trimmed = absl::StripLeadingAsciiWhitespace(trimmed);
      }
    }
    if (!trimmed.empty() && (trimmed[0] == 'R' || trimmed[0] == 'r')) {
      std::string_view rest = trimmed;
      rest.remove_prefix(1);
      rest = absl::StripLeadingAsciiWhitespace(rest);
      if (absl::StartsWith(rest, ":")) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace
CommandHandler::CommandHandler(Database* db, Orchestrator* orchestrator, OAuthHandler* oauth_handler,
                               std::string google_api_key, std::string openai_api_key)
    : db_(db),
      orchestrator_(orchestrator),
      oauth_handler_(oauth_handler),
      google_api_key_(std::move(google_api_key)),
      openai_api_key_(std::move(openai_api_key)) {
  RegisterCommands();
}
void CommandHandler::RegisterCommands() {
  commands_.reserve(64);  // Allocate enough bucket space up front
  commands_["/help"] = [this](CommandArgs& args) { return HandleHelp(args); };
  commands_["/exit"] = [this](CommandArgs& args) { return HandleExit(args); };
  commands_["/edit"] = [this](CommandArgs& args) { return HandleEdit(args); };
  commands_["/message"] = [this](CommandArgs& args) { return HandleMessage(args); };
  commands_["/undo"] = [this](CommandArgs& args) { return HandleUndo(args); };
  commands_["/context"] = [this](CommandArgs& args) { return HandleContext(args); };
  commands_["/tool"] = [this](CommandArgs& args) { return HandleTool(args); };
  commands_["/skill"] = [this](CommandArgs& args) { return HandleSkill(args); };
  commands_["/session"] = [this](CommandArgs& args) { return HandleSession(args); };
  commands_["/stats"] = [this](CommandArgs& args) { return HandleStats(args); };
  commands_["/models"] = [this](CommandArgs& args) { return HandleModels(args); };
  commands_["/exec"] = [this](CommandArgs& args) { return HandleExec(args); };
  commands_["/schema"] = [this](CommandArgs& args) { return HandleSchema(args); };
  commands_["/model"] = [this](CommandArgs& args) { return HandleModel(args); };
  commands_["/throttle"] = [this](CommandArgs& args) { return HandleThrottle(args); };
  commands_["/agents_md"] = [this](CommandArgs& args) { return HandleAgentsMd(args); };
  commands_["/review"] = [this](CommandArgs& args) { return HandleReview(args); };
  commands_["/feedback"] = [this](CommandArgs& args) { return HandleFeedback(args); };
  commands_["/mode"] = [this](CommandArgs& args) { return HandleMode(args); };
  for (const auto& def : GetCommandDefinitions()) {
    auto it = commands_.find(def.name);
    if (it != commands_.end()) {
      auto handler = it->second;  // copy the handler out to the stack
      for (const auto& alias : def.aliases) {
        commands_[alias] = handler;
      }
      if (!def.sub_commands.empty()) {
        sub_commands_[def.name] = def.sub_commands;
      }
    }
  }
}
std::vector<std::string> CommandHandler::GetCommandNames() const {
  std::vector<std::string> names;
  for (const auto& [name, _] : commands_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}
CommandHandler::Result CommandHandler::Handle(std::string& input, std::string& session_id,
                                              std::vector<std::string>& active_skills,
                                              std::function<void()> show_help_fn,
                                              const std::vector<std::string>& selected_groups) {
  std::string trimmed = std::string(absl::StripLeadingAsciiWhitespace(input));
  if (trimmed.empty()) return Result::NOT_A_COMMAND;
  if (trimmed[0] != '/') {
    return Result::NOT_A_COMMAND;
  }
  std::vector<std::string> parts = absl::StrSplit(trimmed, absl::MaxSplits(' ', 1));
  std::string cmd = parts[0];
  std::string args_str = (parts.size() > 1) ? parts[1] : "";
  auto it = commands_.find(cmd);
  if (it != commands_.end()) {
    LOG(INFO) << "Dispatching command: " << cmd << " (args: " << args_str << ")";
    CommandArgs args{input, session_id, active_skills, show_help_fn, selected_groups, args_str};
    return it->second(args);
  }
  std::cerr << "Unknown command: " << cmd << std::endl;
  return Result::UNKNOWN;
}
CommandHandler::Result CommandHandler::HandleHelp(CommandArgs& args) {
  args.show_help_fn();
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleExit([[maybe_unused]] CommandArgs& args) { return Result::HANDLED; }
CommandHandler::Result CommandHandler::HandleEdit(CommandArgs& args) {
  std::string edited = TriggerEditor("", ".txt");
  if (edited.empty()) return Result::HANDLED;
  args.input = edited;
  return Result::PROCEED_TO_LLM;
}
CommandHandler::Result CommandHandler::HandleMessage(CommandArgs& args) {
  std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = sub_parts[0];
  std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
  if (sub_cmd == "list") {
    int n = 10;
    if (!sub_args.empty() && !absl::SimpleAtoi(sub_args, &n)) {
      std::cerr << "Invalid number: " << sub_args << std::endl;
      return Result::HANDLED;
    }
    std::string sql =
        "SELECT m1.group_id, m1.content as prompt, MAX(m2.tokens) as tokens "
        "FROM messages m1 "
        "LEFT JOIN messages m2 ON m1.group_id = m2.group_id AND m2.role = 'assistant' "
        "WHERE m1.session_id = ? AND m1.role = 'user' "
        "GROUP BY m1.group_id ORDER BY m1.created_at DESC LIMIT " +
        std::to_string(n);
    auto res = db_->Query(sql, {args.session_id});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && j.is_array()) {
        std::string md = absl::Substitute("### Message History (Last $0)\n\n", n);
        md += "| Group ID | User Prompt Snippet | Assistant Tokens |\n";
        md += "| :--- | :--- | :---: |\n";
        for (const auto& row : j) {
          std::string prompt = json_get_or(row, "prompt", std::string{});
          std::string escaped_prompt = absl::StrReplaceAll(prompt, {{"|", "\\|"}, {"\n", " "}});
          if (escaped_prompt.length() > 50) escaped_prompt = escaped_prompt.substr(0, 47) + "...";
          std::string tokens_str = "N/A";
          if (row.contains("tokens") && !row["tokens"].is_null()) {
            tokens_str = std::to_string(json_get_or(row, "tokens", 0));
          }
          md += absl::Substitute("| `$0` | $1 | $2 |\n", json_get_or(row, "group_id", std::string{}), escaped_prompt,
                                 tokens_str);
        }
        PrintMarkdown(md);
      }
    }
  } else if (sub_cmd == "view" || sub_cmd == "show") {
    auto res =
        db_->Query("SELECT role, content, tokens FROM messages WHERE group_id = ? ORDER BY created_at ASC", {sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        std::string md = absl::Substitute("### Interaction Group: `$0` \n\n", sub_args);
        for (const auto& m : j) {
          std::string role = m.value("role", "unknown");
          md += absl::Substitute("#### $0", role);
          if (m.contains("tokens") && !m["tokens"].is_null() && json_get_or(m, "tokens", 0) > 0) {
            md += absl::Substitute(" ($0 tokens)", json_get_or(m, "tokens", 0));
          }
          md += "\n" + m.value("content", "") + "\n\n";
        }
        PrintMarkdown(md);
      }
    }
  } else if (sub_cmd == "remove") {
    HandleStatus(db_->Execute("DELETE FROM messages WHERE group_id = ?", {sub_args}));
    std::cout << "Message group " << sub_args << " deleted." << std::endl;
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleUndo(CommandArgs& args) {
  auto gid_or = db_->GetLastGroupId(args.session_id);
  if (gid_or.ok()) {
    std::string gid = *gid_or;
    HandleStatus(db_->Execute("DELETE FROM messages WHERE group_id = ?", {gid}));
    std::cout << "Undid last interaction (Group ID: " + gid + ")" << std::endl;
    if (orchestrator_) {
      auto status = orchestrator_->RebuildContext(args.session_id);
      if (status.ok())
        std::cout << "Context rebuilt." << std::endl;
      else
        HandleStatus(status, "Error rebuilding context");
    }
  } else {
    std::cout << "Nothing to undo." << std::endl;
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleContext(CommandArgs& args) {
  std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = sub_parts[0];
  std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
  if (sub_cmd == "window") {
    int n = sub_args.empty() ? 0 : std::atoi(sub_args.c_str());
    HandleStatus(db_->SetContextWindow(args.session_id, n));
    if (n > 0)
      std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
    else if (n == 0)
      std::cout << "Full Context Mode (infinite buffer)." << std::endl;
    else
      std::cout << "Context Hidden (None)." << std::endl;
    return Result::HANDLED;
  }
  if (sub_cmd == "rebuild") {
    if (orchestrator_) {
      auto status = orchestrator_->RebuildContext(args.session_id);
      if (status.ok())
        std::cout << "Context rebuilt from history." << std::endl;
      else
        HandleStatus(status, "Error");
    } else {
      std::cerr << "Orchestrator not available for rebuilding context." << std::endl;
    }
    return Result::HANDLED;
  }
  if (sub_cmd == "show") {
    auto s = db_->GetContextSettings(args.session_id);
    std::stringstream ss;
    ss << "## Context Status\n";
    ss << "Session: " << args.session_id << "\n";
    ss << "Window Size: ";
    ss << (s.ok() ? (s->size == 0 ? "Infinite" : std::to_string(s->size)) : "Error");
    ss << "\n";
    if (!args.active_skills.empty()) {
      ss << "Active Skills: " << absl::StrJoin(args.active_skills, ", ") << std::endl;
    }
    if (orchestrator_) {
      auto prompt_or = orchestrator_->AssemblePrompt(args.session_id, args.active_skills);
      if (prompt_or.ok()) {
        DisplayAssembledContext(prompt_or->dump());
      }
    }
    return Result::HANDLED;
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleTool(CommandArgs& args) {
  std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = sub_parts[0];
  std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
  if (sub_cmd == "list") {
    auto res = db_->Query("SELECT name, description, is_enabled FROM tools");
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && j.is_array()) {
        std::string md = "### Available Tools\n\n";
        md += "| Name | Description | Enabled |\n";
        md += "| :--- | :--- | :---: |\n";
        for (const auto& row : j) {
          md += absl::Substitute("| `$0` | $1 | $2 |\n", json_get_or(row, "name", std::string{}),
                                 json_get_or(row, "description", std::string{}),
                                 json_get_or(row, "is_enabled", 1) ? "✅" : "❌");
        }
        PrintMarkdown(md);
      }
    }
  } else if (sub_cmd == "show") {
    auto res = db_->Query("SELECT name, description, json_schema FROM tools WHERE name = ?", {sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        std::string md = absl::Substitute("### Tool: $0\n\n", json_get_or(j[0], "name", std::string{}));
        md += "**Description**: " + json_get_or(j[0], "description", std::string{}) + "\n\n";
        md += "**JSON Schema**:\n```json\n" + json_get_or(j[0], "json_schema", std::string{"{}"}) + "\n```\n";
        PrintMarkdown(md);
      }
    }
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleSkill(CommandArgs& args) {
  std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = sub_parts[0];
  std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
  if (sub_cmd == "list") {
    auto res = db_->Query("SELECT id, name, description, activation_count FROM skills");
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && j.is_array()) {
        std::string md = "### Skills\n\n";
        md += "| ID | Name | Description | Activations | Status |\n";
        md += "| :---: | :--- | :--- | :---: | :---: |\n";
        for (const auto& row : j) {
          std::string description = json_get_or(row, "description", std::string{});
          description = absl::StrReplaceAll(description, {{"|", "\\|"}, {"\n", " "}});
          if (description.length() > 60) {
            description = description.substr(0, 57) + "...";
          }
          bool active = std::find(args.active_skills.begin(), args.active_skills.end(),
                                  json_get_or(row, "name", std::string{})) != args.active_skills.end();
          md += absl::Substitute("| $0 | **$1** | $2 | $3 | $4 |\n", json_get_or(row, "id", 0),
                                 json_get_or(row, "name", std::string{}), description,
                                 json_get_or(row, "activation_count", 0), active ? "✓" : "");
        }
        PrintMarkdown(md);
      }
    } else {
      HandleStatus(res.status(), "Database error");
    }
  } else if (sub_cmd == "activate") {
    auto res = db_->Query("SELECT name FROM skills WHERE id = ? OR name = ?", {sub_args, sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        std::string name = j[0]["name"];
        if (std::find(args.active_skills.begin(), args.active_skills.end(), name) == args.active_skills.end()) {
          args.active_skills.push_back(name);
          (void)db_->SetActiveSkills(args.session_id, args.active_skills);
          (void)db_->IncrementSkillActivationCount(name);
        }
        std::cout << icons::Skill << " Skill '" << name << "' activated." << std::endl;
      } else {
        std::cerr << icons::Error << " Skill not found: " << sub_args << std::endl;
      }
    }
  } else if (sub_cmd == "deactivate") {
    auto res = db_->Query("SELECT name FROM skills WHERE id = ? OR name = ?", {sub_args, sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        std::string name = j[0]["name"];
        args.active_skills.erase(std::remove(args.active_skills.begin(), args.active_skills.end(), name),
                                 args.active_skills.end());
        (void)db_->SetActiveSkills(args.session_id, args.active_skills);
        std::cout << icons::Skill << " Skill '" << name << "' deactivated." << std::endl;
      }
    }
  } else if (sub_cmd == "show") {
    auto res = db_->Query("SELECT name, description, system_prompt_patch FROM skills WHERE name = ? OR id = ?",
                          {sub_args, sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        std::cout << "Skill: " << json_get_or(j[0], "name", std::string{}) << std::endl;
        std::cout << "Description: " << json_get_or(j[0], "description", std::string{}) << std::endl;
        std::cout << "Patch:\n" << json_get_or(j[0], "system_prompt_patch", std::string{}) << std::endl;
      }
    }
  } else if (sub_cmd == "edit") {
    auto res = db_->Query(
        "SELECT id, name, description, system_prompt_patch, activation_count FROM skills WHERE name = ? OR id = ?",
        {sub_args, sub_args});
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && !j.empty()) {
        auto& skill_data = j[0];
        int id = json_get_or(skill_data, "id", 0);
        Database::Skill skill{id, skill_data["name"], skill_data["description"], skill_data["system_prompt_patch"],
                              json_get_or(skill_data, "activation_count", 0)};
        std::string initial_md = SkillToMarkdown(skill);
        std::string edited_md = TriggerEditor(initial_md, ".md");
        if (absl::StripAsciiWhitespace(edited_md).empty()) {
          std::cout << "Empty content. Deleting skill..." << std::endl;
          HandleStatus(db_->DeleteSkill(std::to_string(id)));
          return Result::HANDLED;
        }
        if (edited_md != initial_md) {
          Database::Skill s = MarkdownToSkill(edited_md, id);
          s.activation_count = skill.activation_count;
          auto status = db_->UpdateSkill(s);
          HandleStatus(status);
          if (status.ok()) std::cout << "Skill updated." << std::endl;
        } else {
          std::cout << "No changes made." << std::endl;
        }
      } else {
        std::cerr << "Skill not found: " << sub_args << std::endl;
      }
    } else {
      HandleStatus(res.status(), "Database error");
    }
  } else if (sub_cmd == "delete") {
    HandleStatus(db_->DeleteSkill(sub_args));
    std::cout << "Skill deleted." << std::endl;
  } else if (sub_cmd == "add") {
    std::string template_md = absl::Substitute("# Name: $0\n# Description: \n\n# System Prompt Patch\n", sub_args);
    std::string edited_md = TriggerEditor(template_md, ".md");
    if (!absl::StripAsciiWhitespace(edited_md).empty()) {
      Database::Skill s = MarkdownToSkill(edited_md, 0);
      auto status = db_->RegisterSkill(s);
      HandleStatus(status);
      if (status.ok()) std::cout << "Skill added." << std::endl;
    }
  }
  return Result::HANDLED;
}
/**
 * @brief Handles session management commands (/session).
 *
 * Supports:
 * - list: Lists all sessions.
 * - switch <id>: Switches the current active session.
 * - remove <id>: Deletes a session and its history.
 * - clear: Clears history/state for the current session.
 *
 * @param args Command arguments containing the sub-command and optional session ID.
 */
CommandHandler::Result CommandHandler::HandleSession(CommandArgs& args) {
  std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = sub_parts[0];
  std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
  if (sub_cmd == "list") {
    auto res = db_->Query("SELECT DISTINCT session_id FROM messages UNION SELECT DISTINCT id FROM sessions");
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && j.is_array()) {
        std::string md = "### Sessions\n\n";
        md += "| Status | Session ID |\n";
        md += "| :---: | :--- |\n";
        for (const auto& row : j) {
          std::string sid = json_get_or(row, "session_id", json_get_or(row, "id", std::string{}));
          bool active = (sid == args.session_id);
          md += absl::Substitute("| $0 | $1 |\n", active ? "✓" : "", sid);
        }
        PrintMarkdown(md);
      }
    }
  } else if (sub_cmd == "switch") {
    args.session_id = sub_args;
    std::cout << "Session switched to: " << sub_args << std::endl;
    if (orchestrator_) (void)orchestrator_->RebuildContext(args.session_id);
  } else if (sub_cmd == "remove") {
    HandleStatus(db_->DeleteSession(sub_args));
    std::cout << "Session " << sub_args << " deleted." << std::endl;
    if (args.session_id == sub_args) {
      args.session_id = "default_session";
      std::cout << "Returning to default_session." << std::endl;
    }
  } else if (sub_cmd == "clear") {
    HandleStatus(db_->DeleteSession(args.session_id));
    std::cout << "Session " << args.session_id << " history and state cleared." << std::endl;
    if (orchestrator_) (void)orchestrator_->RebuildContext(args.session_id);
  } else if (sub_cmd == "clone") {
    if (sub_args.empty()) {
      std::cout << "Usage: /session clone <new_session_name>" << std::endl;
    } else {
      const std::string& new_session_id = sub_args;
      auto status = db_->CloneSession(args.session_id, new_session_id);
      if (status.ok()) {
        std::cout << "Cloned session '" << args.session_id << "' to '" << new_session_id << "'." << std::endl;
        args.session_id = new_session_id;
        std::cout << "Switched to session: " << new_session_id << std::endl;
        if (orchestrator_) (void)orchestrator_->RebuildContext(new_session_id);
      } else {
        HandleStatus(status, "cloning session");
      }
    }
  } else if (sub_cmd == "scratchpad") {
    // Scratchpad is a per-session persistent markdown-based text area.
    // It allows maintaining a long-running plan or state that isn't lost
    // when the context window shifts.
    std::vector<std::string> scratch_parts = absl::StrSplit(sub_args, absl::MaxSplits(' ', 1));
    std::string scratch_op = scratch_parts[0];
    if (scratch_op == "read") {
      auto res = db_->GetScratchpad(args.session_id);
      if (res.ok()) {
        std::string md = absl::Substitute("## Scratchpad [$0]\n\n", args.session_id);
        md += *res;
        PrintMarkdown(md);
      } else {
        std::cout << "Scratchpad is empty or session not found." << std::endl;
      }
    } else if (scratch_op == "edit") {
      auto current = db_->GetScratchpad(args.session_id);
      std::string initial = current.ok() ? *current : "";
      std::string updated = TriggerEditor(initial, ".md");
      if (!updated.empty()) {
        HandleStatus(db_->UpdateScratchpad(args.session_id, updated));
        std::cout << "Scratchpad updated." << std::endl;
      } else {
        std::cout << "Scratchpad not updated (empty or editor error)." << std::endl;
      }
    } else {
      std::cout << "Unknown scratchpad operation: " << scratch_op << ". Use read or edit." << std::endl;
    }
  } else {
    std::cout << "Unknown session command: " << sub_cmd << ". Try: list, switch, remove, clear, clone, scratchpad"
              << std::endl;
  }
  return Result::HANDLED;
}
/**
 * @brief Displays usage statistics and Gemini user quota.
 *
 * Fetches token usage from the local database grouped by model.
 * If the provider is Gemini and OAuth is active, it also fetches and displays
 * real-time quota information from the Google API.
 *
 * @param args Command arguments providing the session ID.
 */
CommandHandler::Result CommandHandler::HandleStats(CommandArgs& args) {
  auto res = db_->Query(
      "SELECT model, SUM(prompt_tokens) as prompt, SUM(completion_tokens) as completion, "
      "SUM(prompt_tokens + completion_tokens) as total FROM usage "
      "WHERE session_id = ? GROUP BY model",
      {args.session_id});
  if (res.ok()) {
    std::string md = "## Usage Stats for Session [" + args.session_id + "]\n\n";
    auto j = json_parse(*res).value_or(nlohmann::json::object());
    if (!j.is_discarded() && j.is_array() && !j.empty()) {
      md += "| Model | Prompt | Completion | Total |\n";
      md += "| :--- | :---: | :---: | :---: |\n";
      for (const auto& row : j) {
        md += absl::Substitute("| $0 | $1 | $2 | $3 |\n", json_get_or(row, "model", std::string{"unknown"}),
                               json_get_or(row, "prompt", 0), json_get_or(row, "completion", 0),
                               json_get_or(row, "total", 0));
      }
      md += "\n";
      PrintMarkdown(md);
    } else {
      std::cout << "No usage data for session [" << args.session_id << "]" << std::endl;
    }
  }
  auto tools_res = db_->Query("SELECT name, call_count FROM tools WHERE call_count > 0 ORDER BY call_count DESC");
  if (tools_res.ok()) {
    auto j = json_parse(*tools_res).value_or(nlohmann::json::object());
    if (!j.is_discarded() && j.is_array() && !j.empty()) {
      std::string md = "### Tool Usage (All-time)\n\n";
      md += "| Tool | Calls |\n";
      md += "| :--- | :---: |\n";
      for (const auto& row : j) {
        md += absl::Substitute("| $0 | $1 |\n", json_get_or(row, "name", std::string{"unknown"}),
                               json_get_or(row, "call_count", 0));
      }
      md += "\n";
      PrintMarkdown(md);
    }
  }
  auto skills_res =
      db_->Query("SELECT name, activation_count FROM skills WHERE activation_count > 0 ORDER BY activation_count DESC");
  if (skills_res.ok()) {
    auto j = json_parse(*skills_res).value_or(nlohmann::json::object());
    if (!j.is_discarded() && j.is_array() && !j.empty()) {
      std::string md = "### Skill Activations (All-time)\n\n";
      md += "| Skill | Activations |\n";
      md += "| :--- | :---: |\n";
      for (const auto& row : j) {
        md += absl::Substitute("| $0 | $1 |\n", json_get_or(row, "name", std::string{"unknown"}),
                               json_get_or(row, "activation_count", 0));
      }
      md += "\n";
      PrintMarkdown(md);
    }
  }
  if (orchestrator_ && orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ &&
      oauth_handler_->IsEnabled()) {
    auto token_or = oauth_handler_->GetValidToken();
    if (token_or.ok()) {
      auto quota_or = orchestrator_->GetQuota(*token_or);
      if (quota_or.ok() && quota_or->is_object()) {
        std::string md = "### Gemini User Quota\n\n";
        if (quota_or->contains("buckets") && (*quota_or)["buckets"].is_array() && !(*quota_or)["buckets"].empty()) {
          md += "| Model ID | Remaining | % | Reset Time | Type |\n";
          md += "| :--- | :--- | :---: | :--- | :--- |\n";
          for (const auto& b : (*quota_or)["buckets"]) {
            if (!b.is_object()) continue;
            double fraction = json_get_or(b, "remainingFraction", 0.0);
            md += absl::Substitute("| `$0` | $1 | $2% | $3 | $4 |\n", json_get_or(b, "modelId", std::string{"N/A"}),
                                   json_get_or(b, "remainingAmount", std::string{"N/A"}), static_cast<int>(fraction * 100),
                                   json_get_or(b, "resetTime", std::string{"N/A"}),
                                   json_get_or(b, "tokenType", std::string{"N/A"}));
          }
          PrintMarkdown(md);
        } else {
          std::cout << "No quota buckets found." << std::endl;
        }
      } else {
        std::cout << "Could not fetch quota: " << quota_or.status().message() << std::endl;
      }
    }
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleModels(CommandArgs& args) {
  if (!orchestrator_) return Result::HANDLED;
  std::string api_key =
      (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI) ? google_api_key_ : openai_api_key_;
  if (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ && oauth_handler_->IsEnabled()) {
    auto token_or = oauth_handler_->GetValidToken();
    if (token_or.ok()) api_key = *token_or;
  }
  auto models_or = orchestrator_->GetModels(api_key);
  if (!models_or.ok()) {
    HandleStatus(models_or.status(), "Error fetching models");
    return Result::HANDLED;
  }
  std::cout << "Available Models:" << std::endl;
  for (const auto& m : *models_or) {
    if (args.args.empty() || absl::StrContains(m.id, args.args)) {
      std::cout << " - " << m.id << " (" << m.name << ")" << std::endl;
    }
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleExec(CommandArgs& args) {
  if (args.args.empty()) {
    std::cerr << "Usage: /exec <command>" << std::endl;
    return Result::HANDLED;
  }
  LOG(INFO) << "Executing shell command: " << args.args;
  std::cout << "Executing: " << args.args << std::endl;
  int res = std::system(args.args.c_str());
  LOG(INFO) << "Shell command exited with code " << res;
  std::cout << "Exit code: " << res << std::endl;
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleSchema([[maybe_unused]] CommandArgs& args) {
  auto res = db_->Query("SELECT sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
  if (res.ok()) {
    auto j = json_parse(*res).value_or(nlohmann::json::object());
    if (!j.is_discarded()) {
      for (const auto& row : j) {
        std::cout << json_get_or(row, "sql", std::string{}) << ";\n" << std::endl;
      }
    }
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleModel(CommandArgs& args) {
  if (args.args.empty()) {
    std::cout << "Current model: " << orchestrator_->GetModel() << std::endl;
  } else {
    orchestrator_->Update().WithModel(args.args).BuildInto(orchestrator_);
    std::cout << "Model set to: " << args.args << std::endl;
  }
  return Result::HANDLED;
}
CommandHandler::Result CommandHandler::HandleThrottle(CommandArgs& args) {
  if (args.args.empty()) {
    std::cout << "Current throttle: " << orchestrator_->GetThrottle() << " seconds." << std::endl;
  } else {
    int n;
    if (absl::SimpleAtoi(args.args, &n)) {
      orchestrator_->Update().WithThrottle(n).BuildInto(orchestrator_);
      std::cout << "Throttle set to " << n << " seconds." << std::endl;
    } else {
      std::cerr << "Invalid throttle value: " << args.args << std::endl;
    }
  }
  return Result::HANDLED;
}
std::string CommandHandler::TriggerEditor(const std::string& initial_content, const std::string& extension) {
  return slop::OpenInEditor(initial_content, extension);
}
absl::StatusOr<std::string> CommandHandler::ExecuteCommand(const std::string& command) {
  auto res = slop::RunCommand(command);
  if (!res.ok()) return res.status();
  std::string output = res->stdout_out;
  if (!res->stderr_out.empty()) {
    if (!output.empty() && output.back() != '\n') output += "\n";
    output += "### STDERR\n" + res->stderr_out;
  }
  if (res->exit_code != 0) {
    return absl::InternalError(absl::StrCat("Command failed with status ", res->exit_code, ": ", output));
  }
  return output;
}
CommandHandler::Result CommandHandler::HandleReview(CommandArgs& args) {
  auto git_check = ExecuteCommand("git rev-parse --is-inside-work-tree");
  if (!git_check.ok() || !absl::StrContains(*git_check, "true")) {
    std::cerr << "Error: /review is only available inside a git repository." << std::endl;
    return Result::HANDLED;
  }
  std::vector<std::string> tokens = absl::StrSplit(args.args, ' ', absl::SkipEmpty());
  // --- Dashboard ---
  if (tokens.empty()) {
    std::cout << "--- Review Dashboard ---" << std::endl;
    // Session Status
    auto status_res = ExecuteCommand("git status --porcelain");
    if (status_res.ok() && !status_res->empty()) {
      std::vector<std::string> lines = absl::StrSplit(*status_res, '\n', absl::SkipEmpty());
      size_t file_count = lines.size();
      std::cout << "[Session] Uncommitted changes detected in " << file_count << " file(s)." << std::endl;
      std::cout << "          Use '/review session' to review." << std::endl;
    } else {
      std::cout << "[Session] No uncommitted changes." << std::endl;
    }
    // Mail Status
    auto branch_res = ExecuteCommand("git rev-parse --abbrev-ref HEAD");
    if (branch_res.ok()) {
      std::string branch = *branch_res;
      absl::StripAsciiWhitespace(&branch);
      if (absl::StartsWith(branch, "slop/staging/")) {
        std::string base = ResolveBaseBranch(branch);
        auto rev_res = ExecuteCommand("git rev-list --count " + base + "..HEAD");
        if (rev_res.ok()) {
          std::string count = *rev_res;
          absl::StripAsciiWhitespace(&count);
          std::cout << "[Mail]    Active series: " << count << " patch(es) in branch '" << branch << "'." << std::endl;
          std::cout << "          Use '/review mail' to review the whole series." << std::endl;
          std::cout << "          Use '/review mail <n>' for a specific patch." << std::endl;
        }
      } else {
        std::cout << "[Mail]    No active staging branch." << std::endl;
      }
    }
    std::cout << "\n[Git]     Review any git reference (branch, hash, or HEAD~n)." << std::endl;
    std::cout << "          Use '/review git <ref>'." << std::endl;
    return Result::HANDLED;
  }
  // Handle mail review
  if (tokens[0] == "mail") {
    std::string base;
    std::vector<std::string> patch_args = absl::StrSplit(args.args, ' ', absl::SkipEmpty());
    int patch_idx = -1;
    if (patch_args.size() > 1 && patch_args[1] != "approve") {
      if (!absl::SimpleAtoi(patch_args[1], &patch_idx)) {
        base = patch_args[1];
      }
    }
    if (base.empty()) {
      auto branch_res = ExecuteCommand("git rev-parse --abbrev-ref HEAD");
      std::string branch = branch_res.ok() ? *branch_res : "";
      absl::StripAsciiWhitespace(&branch);
      base = ResolveBaseBranch(branch);
    }
    std::string rev_cmd = "git rev-list --reverse " + base + "..HEAD";
    auto rev_res = ExecuteCommand(rev_cmd);
    // Diagnostic check: are we on the base branch?
    auto current_res = ExecuteCommand("git rev-parse --abbrev-ref HEAD");
    std::string current_branch;
    if (current_res.ok()) {
      current_branch = *current_res;
      absl::StripAsciiWhitespace(&current_branch);
    }
    // Handle approval
    if (patch_args.size() > 1 && patch_args[1] == "approve") {
      if (!absl::StartsWith(current_branch, "slop/staging/")) {
        std::cerr << "Error: Approval can only be performed on a staging branch." << std::endl;
        return Result::HANDLED;
      }
      if (!rev_res.ok() || rev_res->empty()) {
        std::cerr << "Error: No patches found to approve in range " << base << "..HEAD." << std::endl;
        return Result::HANDLED;
      }
      auto head_res = ExecuteCommand("git rev-parse HEAD");
      if (!head_res.ok()) {
        std::cerr << "Error: Failed to get current HEAD hash." << std::endl;
        return Result::HANDLED;
      }
      std::string head_hash = *head_res;
      absl::StripAsciiWhitespace(&head_hash);
      auto status = db_->SetPatchApproval(current_branch, head_hash);
      if (status.ok()) {
        std::cout << "Approved patchset for branch '" << current_branch << "' at hash " << head_hash << std::endl;
        args.input = absl::Substitute(
            "I have approved the patchset for branch '$0' at hash $1. Please proceed with git_finalize_series.",
            current_branch, head_hash);
        return Result::PROCEED_TO_LLM;
      }
      std::cerr << "Error: Failed to save approval to database: " << status.message() << std::endl;
      return Result::HANDLED;
    }
    if (!rev_res.ok() || rev_res->empty()) {
      std::cout << "No patches found to review in range " << base << "..HEAD." << std::endl;
      if (current_branch == base) {
        std::cout << "Tip: You are currently on the base branch '" << base << "'. "
                  << "Commits here are not treated as patches. Use '/mode mail' to start a staging branch."
                  << std::endl;
      } else if (absl::StartsWith(current_branch, "slop/staging/")) {
        std::cout << "Tip: If you are on a staging branch, in mail mode, but do not see patches yet, "
                  << "ask the agent to git_commit_patch the changes as a patch" << std::endl;
      }
      return Result::HANDLED;
    }
    std::vector<std::string> commits = absl::StrSplit(*rev_res, '\n', absl::SkipEmpty());
    std::cout << "Reviewing " << commits.size() << " patch(es) in range " << base << "..HEAD" << std::endl;
    std::string review_content;
    if (patch_idx > 0 && patch_idx <= static_cast<int>(commits.size())) {
      std::string hash = commits[patch_idx - 1];
      auto show_res = ExecuteCommand("git show -s --pretty=format:\"%s%n%b\" " + hash);
      auto diff_res = ExecuteCommand("git show -p " + hash);
      review_content = "### Patch [" + std::to_string(patch_idx) + "/" + std::to_string(commits.size()) +
                       "]: " + (show_res.ok() ? *show_res : "") + " ###\n" + (diff_res.ok() ? *diff_res : "");
    } else {
      for (size_t i = 0; i < commits.size(); ++i) {
        auto show_res = ExecuteCommand("git show -s --pretty=format:\"%s%n%b\" " + commits[i]);
        auto diff_res = ExecuteCommand("git show -p " + commits[i]);
        review_content += "### Patch [" + std::to_string(i + 1) + "/" + std::to_string(commits.size()) +
                          "]: " + (show_res.ok() ? *show_res : "") + " ###\n" + (diff_res.ok() ? *diff_res : "") +
                          "\n\n";
      }
    }
    std::string initial_content =
        "# --- MAIL REVIEW ---\n"
        "# Add your review comments on new lines starting with 'R:'\n"
        "# Example:\n"
        "# R: Please refactor this function to be more concise.\n"
        "#\n"
        "# Save and exit to send comments to the LLM.\n"
        "# --------------------\n\n" +
        review_content;
    std::string feedback = TriggerEditor(initial_content, ".patch");
    if (feedback.empty() || feedback == initial_content) {
      return Result::HANDLED;
    }
    if (HasReviewComments(feedback)) {
      args.input = "I have reviewed the patches. Here are my comments:\n\n" + feedback +
                   "\n\nPlease address only the comments marked with 'R:' in the patches above.";
      return Result::PROCEED_TO_LLM;
    }
    std::cout << "No 'R:' comments found. Ignoring review." << std::endl;
    return Result::HANDLED;
  }
  std::string diff_cmd;
  bool is_historical = false;
  if (tokens[0] == "session") {
    diff_cmd = "git diff";
    for (size_t i = 1; i < tokens.size(); ++i) {
      absl::StrAppend(&diff_cmd, " ", tokens[i]);
    }
    is_historical = false;
  } else if (tokens[0] == "git") {
    if (tokens.size() < 2) {
      std::cerr << "Error: /review git requires a reference (e.g., /review git HEAD~1)." << std::endl;
      return Result::HANDLED;
    }
    is_historical = true;
    std::string ref = tokens[1];
    int n;
    if (absl::SimpleAtoi(ref, &n)) {
      diff_cmd = absl::StrCat("git diff HEAD~", n);
    } else {
      diff_cmd = absl::StrCat("git diff ", ref);
    }
    for (size_t i = 2; i < tokens.size(); ++i) {
      absl::StrAppend(&diff_cmd, " ", tokens[i]);
    }
  } else {
    std::cerr << "Unknown review command: " << tokens[0] << std::endl;
    std::cerr << "Use /review for a list of available commands." << std::endl;
    return Result::HANDLED;
  }
  // Handle new files with intent-to-add
  if (!is_historical) {
    auto untracked_or = ExecuteCommand("git ls-files --others --exclude-standard");
    if (untracked_or.ok() && !untracked_or->empty()) {
      std::vector<std::string> files = absl::StrSplit(*untracked_or, '\n', absl::SkipEmpty());
      if (!files.empty()) {
        std::string cmd = "git add -N --";
        for (const auto& file : files) {
          // Simple shell escaping: wrap in single quotes, replace ' with '\''
          std::string escaped = file;
          absl::StrReplaceAll({{"'", "'\\''"}}, &escaped);
          absl::StrAppend(&cmd, " '", escaped, "'");
        }
        auto res = ExecuteCommand(cmd);
        if (!res.ok()) {
          slop::HandleStatus(res.status(), "Failed to stage untracked files");
        }
      }
    }
  }
  auto diff_or = ExecuteCommand(diff_cmd);
  if (!diff_or.ok() || diff_or->empty()) {
    std::cout << "No changes to review." << std::endl;
    if (mail_mode_) {
      std::cout << "Tip: Did you mean to use '/review mail' to review patches in the current series?" << std::endl;
    }
    return Result::HANDLED;
  }
  std::string initial_content =
      "# --- MANUAL REVIEW ---\n"
      "# Add your review comments on new lines starting with 'R:'\n"
      "# Example:\n"
      "# R: Please refactor this function to be more concise.\n"
      "#\n"
      "# Save and exit to send comments to the LLM.\n"
      "# ----------------------\n\n" +
      *diff_or;
  std::string edited = TriggerEditor(initial_content, ".diff");
  if (edited.empty()) {
    LOG(INFO) << "Editor returned no content or failed for review.";
    std::cerr << "Editor returned no content or failed. Ignoring review." << std::endl;
    return Result::HANDLED;
  }
  if (absl::StripAsciiWhitespace(edited) == absl::StripAsciiWhitespace(initial_content)) {
    std::cout << "No changes detected. Ignoring review." << std::endl;
    return Result::HANDLED;
  }
  if (!HasReviewComments(edited)) {
    std::cout << "No 'R:' comments found. Ignoring review." << std::endl;
    return Result::HANDLED;
  }
  args.input = "The user has reviewed the current changes. Here is the diff with their 'R:' comments:\n\n" + edited +
               "\n\nPlease address the instructions marked with 'R:' in the diff above. Do not commit any changes "
               "after addressing.";
  return Result::PROCEED_TO_LLM;
}
CommandHandler::Result CommandHandler::HandleFeedback(CommandArgs& args) {
  auto history_or = db_->GetConversationHistory(args.session_id);
  if (!history_or.ok() || history_or->empty()) {
    std::cout << "No conversation history found." << std::endl;
    return Result::HANDLED;
  }
  const auto& history = *history_or;
  std::optional<Database::Message> last_assistant;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (it->role == "assistant") {
      last_assistant = *it;
      break;
    }
  }
  if (!last_assistant) {
    std::cout << "No assistant message found to provide feedback on." << std::endl;
    return Result::HANDLED;
  }
  std::string assistant_text = MessageParser::ExtractAssistantText(MessageContext(*last_assistant));
  if (assistant_text.empty()) {
    std::cout << "The last assistant message has no text content to provide feedback on." << std::endl;
    return Result::HANDLED;
  }
  std::vector<std::string> lines = absl::StrSplit(assistant_text, '\n');
  std::string initial_content =
      "# --- ASSISTANT MESSAGE FEEDBACK ---\n"
      "# Add your feedback comments on new lines starting with 'R:'\n"
      "# Example:\n"
      "# 10: void some_function() {\n"
      "# R: This function name is too vague.\n"
      "#\n"
      "# Save and exit to send feedback to the LLM.\n"
      "# ----------------------------------\n\n";
  for (size_t i = 0; i < lines.size(); ++i) {
    absl::StrAppend(&initial_content, i + 1, ": ", lines[i], "\n");
  }
  std::string edited = TriggerEditor(initial_content, ".txt");
  if (edited.empty()) {
    LOG(INFO) << "Editor returned no content or failed for feedback.";
    std::cerr << "Editor returned no content or failed. Ignoring feedback." << std::endl;
    return Result::HANDLED;
  }
  if (absl::StripAsciiWhitespace(edited) == absl::StripAsciiWhitespace(initial_content)) {
    std::cout << "No changes detected. Ignoring feedback." << std::endl;
    return Result::HANDLED;
  }
  if (!HasReviewComments(edited)) {
    std::cout << "No 'R:' comments found. Ignoring feedback." << std::endl;
    return Result::HANDLED;
  }
  args.input =
      "The user has provided feedback on your last message. Here is the message with their 'R:' comments:\n\n" +
      edited + "\n\nPlease address the feedback marked with 'R:' in the message above.";
  return Result::PROCEED_TO_LLM;
}
CommandHandler::Result CommandHandler::HandleMode(CommandArgs& args) {
  std::string mode = std::string(absl::StripAsciiWhitespace(args.args));
  if (mode == "mail") {
    // Check if it's a git repo
    auto git_check = ExecuteCommand("git rev-parse --is-inside-work-tree");
    if (!git_check.ok()) {
      std::cout << "Error: Not a git repository. Please run 'git init' first." << std::endl;
      return Result::HANDLED;
    }
    auto status_check = ExecuteCommand("git status --porcelain");
    if (!status_check.ok()) {
      std::cout << "Error: Failed to check git status: " << status_check.status().message() << std::endl;
      return Result::HANDLED;
    }
    if (!absl::StripAsciiWhitespace(*status_check).empty()) {
      std::cout << "Error: Git repository is dirty. The Mail Model requires a clean state because 'git_commit_patch' "
                   "automatically includes all local changes (including untracked files) into your patches."
                << std::endl;
      std::cout << "Please commit, stash, or .gitignore your changes before switching to MAIL mode." << std::endl;
      std::cout << "\nDirty files:\n" << *status_check << std::endl;
      return Result::HANDLED;
    }
    mail_mode_ = true;
    (void)db_->Query("UPDATE settings SET mode = 'mail' WHERE id = 1");
    auto current_branch_res = ExecuteCommand("git rev-parse --abbrev-ref HEAD");
    std::string current_branch = current_branch_res.ok() ? *current_branch_res : "";
    absl::StripAsciiWhitespace(&current_branch);
    std::string base = ResolveBaseBranch(current_branch);
    std::cout << "Switched to MAIL mode." << std::endl;
    std::cout << "  - Modeline: std::slop<MAIL, ...>" << std::endl;
    std::cout << "  - Base Branch: " << base << std::endl;
    std::cout << "  - Workflow: Use /review mail [index] to iterate on patches." << std::endl;
    // Auto-activate patcher skill if it exists
    auto res = db_->Query("SELECT name FROM skills WHERE name = 'patcher'");
    if (res.ok()) {
      auto j = json_parse(*res).value_or(nlohmann::json::object());
      if (!j.is_discarded() && j.is_array() && !j.empty()) {
        bool already_active = false;
        for (const auto& s : args.active_skills) {
          if (s == "patcher") {
            already_active = true;
            break;
          }
        }
        if (!already_active) {
          args.active_skills.emplace_back("patcher");
          (void)db_->SetActiveSkills(args.session_id, args.active_skills);
          std::cout << "Skill 'patcher' auto-activated." << std::endl;
        }
      }
    }
  } else if (mode == "standard" || mode == "default") {
    mail_mode_ = false;
    (void)db_->Query("UPDATE settings SET mode = 'standard' WHERE id = 1");
    std::cout << "Switched to STANDARD mode." << std::endl;
    // Deactivate patcher skill
    auto it = std::remove(args.active_skills.begin(), args.active_skills.end(), "patcher");
    if (it != args.active_skills.end()) {
      args.active_skills.erase(it, args.active_skills.end());
      (void)db_->SetActiveSkills(args.session_id, args.active_skills);
      std::cout << "Skill 'patcher' deactivated." << std::endl;
    }
  } else if (mode.empty()) {
    std::cout << "Current mode: " << (mail_mode_ ? "MAIL" : "STANDARD") << std::endl;
  } else {
    std::cout << "Unknown mode: " << mode << ". Use 'mail' or 'standard'." << std::endl;
  }
  return Result::HANDLED;
}
std::string CommandHandler::SkillToMarkdown(const Database::Skill& skill) {
  return absl::Substitute("# Name: $0\n# Description: $1\n\n# System Prompt Patch\n$2", skill.name, skill.description,
                          skill.system_prompt_patch);
}
Database::Skill CommandHandler::MarkdownToSkill(const std::string& md, int id) {
  Database::Skill s;
  s.id = id;
  bool in_patch = false;
  for (absl::string_view line : absl::StrSplit(md, '\n')) {
    if (!in_patch) {
      absl::string_view line_view = line;
      if (absl::ConsumePrefix(&line_view, "# Name:")) {
        s.name = std::string(absl::StripAsciiWhitespace(line_view));
      } else if (absl::ConsumePrefix(&line_view, "# Description:")) {
        s.description = std::string(absl::StripAsciiWhitespace(line_view));
      } else if (absl::StartsWith(line, "# System Prompt Patch")) {
        in_patch = true;
      }
    } else {
      absl::StrAppend(&s.system_prompt_patch, line, "\n");
    }
  }
  s.system_prompt_patch = std::string(absl::StripAsciiWhitespace(s.system_prompt_patch));
  return s;
}
std::string CommandHandler::ResolveBaseBranch(const std::string& current_branch) {
  // If we are not on a staging branch, then we are on what will be the base branch
  // for any subsequent Mail Model actions.
  if (!current_branch.empty() && !absl::StartsWith(current_branch, "slop/staging/")) {
    return current_branch;
  }
  // The ONLY source of truth: the staging_branches table
  auto results = db_->Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?;", {current_branch});
  if (results.ok()) {
    auto j_opt = json_parse(*results);
    if (j_opt && j_opt->is_array() && !j_opt->empty()) {
      auto parent = json_get<std::string>((*j_opt)[0], "parent_branch");
      if (parent) return *parent;
    }
  }
  // Final fallback for non-staging or missing DB entry
  return "main";
}
CommandHandler::Result CommandHandler::HandleAgentsMd(CommandArgs& args) {
  std::vector<std::string> parts = absl::StrSplit(args.args, ' ', absl::SkipEmpty());
  if (parts.empty()) {
    args.show_help_fn();
    return Result::HANDLED;
  }
  std::string sub = parts[0];
  if (sub == "show") {
    auto content_or = orchestrator_->GetDatabase()->GetAgentMd(orchestrator_->GetActiveAgentMdPath());
    if (!content_or.ok() || content_or->empty()) {
      std::cout << "No AGENTS.md context loaded." << std::endl;
      return Result::HANDLED;
    }
    std::cout << "--- " << orchestrator_->GetActiveAgentMdPath() << " ---" << std::endl;
    std::cout << *content_or << std::endl;
    std::cout << "--- END ---" << std::endl;
  } else if (sub == "reload") {
    std::string path = (parts.size() > 1) ? parts[1] : "./AGENTS.md";
    auto status = orchestrator_->LoadAgentMd(path);
    if (!status.ok()) {
      std::cout << "Error loading AGENTS.md: " << status.message() << std::endl;
    } else {
      std::cout << "Successfully loaded AGENTS.md from " << path << std::endl;
    }
  } else {
    args.show_help_fn();
  }
  return Result::HANDLED;
}

}  // namespace slop