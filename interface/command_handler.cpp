#include "command_handler.h"
#include "oauth_handler.h"
#include "orchestrator.h"
#include "ui.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/substitute.h"
#include "nlohmann/json.hpp"

#include "command_definitions.h"
#include "shell_util.h"

namespace slop {

namespace {}  // namespace

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
  commands_["/memo"] = [this](CommandArgs& args) { return HandleMemo(args); };
  commands_["/manual-review"] = [this](CommandArgs& args) { return HandleManualReview(args); };

  for (const auto& def : GetCommandDefinitions()) {
    auto it = commands_.find(def.name);
    if (it != commands_.end()) {
      for (const auto& alias : def.aliases) {
        commands_[alias] = it->second;
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

std::vector<std::string> CommandHandler::GetSubCommands(const std::string& command) const {
  auto it = sub_commands_.find(command);
  if (it != sub_commands_.end()) {
    std::vector<std::string> subs = it->second;
    std::sort(subs.begin(), subs.end());
    return subs;
  }
  return {};
}

CommandHandler::Result CommandHandler::Handle(std::string& input, std::string& session_id,
                                              std::vector<std::string>& active_skills,
                                              std::function<void()> show_help_fn,
                                              const std::vector<std::string>& selected_groups) {
  std::string trimmed = std::string(absl::StripLeadingAsciiWhitespace(input));
  if (trimmed.empty() || trimmed[0] != '/') return Result::NOT_A_COMMAND;

  std::vector<std::string> parts = absl::StrSplit(trimmed, absl::MaxSplits(' ', 1));
  std::string cmd = parts[0];
  std::string args_str = (parts.size() > 1) ? parts[1] : "";

  auto it = commands_.find(cmd);
  if (it != commands_.end()) {
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
  std::string edited = TriggerEditor("");
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
    if (res.ok()) HandleStatus(PrintJsonAsTable(*res));
  } else if (sub_cmd == "view" || sub_cmd == "show") {
    auto res = db_->Query("SELECT role, content, tokens FROM messages WHERE group_id = ? ORDER BY created_at ASC",
                          {sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        std::string out;
        for (const auto& m : j) {
          out += "[" + m["role"].get<std::string>() + "]:";
          if (m.contains("tokens") && !m["tokens"].is_null() && m["tokens"].get<int>() > 0) {
            out += " (" + std::to_string(m["tokens"].get<int>()) + " tokens)";
          }
          out += "\n" + m["content"].get<std::string>() + "\n\n";
        }
        SmartDisplay(out);
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
    ss << "--- CONTEXT STATUS ---\n";
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
        ss << "\n--- ASSEMBLED PROMPT ---" << std::endl;
        ss << prompt_or->dump(2) << std::endl;
      }
    }
    SmartDisplay(ss.str());
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
    if (res.ok()) HandleStatus(PrintJsonAsTable(*res));
  } else if (sub_cmd == "show") {
    auto res = db_->Query("SELECT name, description, json_schema FROM tools WHERE name = ?", {sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        std::cout << "Tool: " << j[0]["name"].get<std::string>() << std::endl;
        std::cout << "Description: " << j[0]["description"].get<std::string>() << std::endl;
        std::cout << "Schema:\n" << j[0]["json_schema"].get<std::string>() << std::endl;
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
    auto res = db_->Query("SELECT id, name, description FROM skills");
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded()) {
        for (auto& row : j) {
          bool active = std::find(args.active_skills.begin(), args.active_skills.end(),
                                  row["name"].get<std::string>()) != args.active_skills.end();
          row["status"] = active ? "ACTIVE" : "inactive";
        }
        HandleStatus(PrintJsonAsTable(j.dump()));
      }
    }
  } else if (sub_cmd == "activate") {
    auto res = db_->Query("SELECT name FROM skills WHERE id = ? OR name = ?", {sub_args, sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        std::string name = j[0]["name"];
        args.active_skills.push_back(name);
        std::cout << "Skill '" << name << "' activated." << std::endl;
      } else {
        std::cerr << "Skill not found: " << sub_args << std::endl;
      }
    }
  } else if (sub_cmd == "deactivate") {
    auto res = db_->Query("SELECT name FROM skills WHERE id = ? OR name = ?", {sub_args, sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        std::string name = j[0]["name"];
        args.active_skills.erase(std::remove(args.active_skills.begin(), args.active_skills.end(), name),
                                 args.active_skills.end());
        std::cout << "Skill '" << name << "' deactivated." << std::endl;
      }
    }
  } else if (sub_cmd == "show") {
    auto res = db_->Query("SELECT name, description, system_prompt_patch FROM skills WHERE name = ? OR id = ?",
                          {sub_args, sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        std::cout << "Skill: " << j[0]["name"].get<std::string>() << std::endl;
        std::cout << "Description: " << j[0]["description"].get<std::string>() << std::endl;
        std::cout << "Patch:\n" << j[0]["system_prompt_patch"].get<std::string>() << std::endl;
      }
    }
  } else if (sub_cmd == "edit") {
    auto res = db_->Query("SELECT id, name, description, system_prompt_patch FROM skills WHERE name = ? OR id = ?",
                          {sub_args, sub_args});
    if (res.ok()) {
      auto j = nlohmann::json::parse(*res, nullptr, false);
      if (!j.is_discarded() && !j.empty()) {
        auto& skill_data = j[0];
        int id = skill_data["id"].get<int>();

        nlohmann::json edit_obj;
        edit_obj["name"] = skill_data["name"];
        edit_obj["description"] = skill_data["description"];
        edit_obj["system_prompt_patch"] = skill_data["system_prompt_patch"];

        std::string edited_json = TriggerEditor(edit_obj.dump(2));
        if (!edited_json.empty()) {
          auto edited_j = nlohmann::json::parse(edited_json, nullptr, false);
          if (!edited_j.is_discarded()) {
            Database::Skill s{
                id, edited_j.value("name", skill_data["name"].get<std::string>()),
                edited_j.value("description", skill_data["description"].get<std::string>()),
                edited_j.value("system_prompt_patch", skill_data["system_prompt_patch"].get<std::string>())};
            HandleStatus(db_->UpdateSkill(s));
            std::cout << "Skill updated." << std::endl;
          } else {
            std::cerr << "Invalid JSON. Update aborted." << std::endl;
          }
        } else {
          std::cout << "No changes made." << std::endl;
        }
      } else {
        std::cerr << "Skill not found: " << sub_args << std::endl;
      }
    }
  } else if (sub_cmd == "delete") {
    HandleStatus(db_->DeleteSkill(sub_args));
    std::cout << "Skill deleted." << std::endl;
  } else if (sub_cmd == "add") {
    std::string name, desc, patch;
    std::cout << "Skill Name: ";
    std::getline(std::cin, name);
    std::cout << "Description: ";
    std::getline(std::cin, desc);
    std::cout << "System Prompt Patch (Ctrl+D to finish):\n";
    std::string line;
    while (std::getline(std::cin, line)) patch += line + "\n";
    Database::Skill s{0, name, desc, patch};
    HandleStatus(db_->RegisterSkill(s));
  }
  return Result::HANDLED;
}

/**
 * @brief Handles session management commands (/session).
 *
 * Supports:
 * - list: Lists all sessions.
 * - activate <id>: Switches the current active session.
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
    if (res.ok()) HandleStatus(PrintJsonAsTable(*res));
  } else if (sub_cmd == "activate") {
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
  } else if (sub_cmd == "scratchpad") {
    // Scratchpad is a per-session persistent markdown-based text area.
    // It allows maintaining a long-running plan or state that isn't lost
    // when the context window shifts.
    std::vector<std::string> scratch_parts = absl::StrSplit(sub_args, absl::MaxSplits(' ', 1));
    std::string scratch_op = scratch_parts[0];
    std::string scratch_val = (scratch_parts.size() > 1) ? scratch_parts[1] : "";

    if (scratch_op == "read") {
      auto res = db_->GetScratchpad(args.session_id);
      if (res.ok()) {
        std::cout << "--- Scratchpad [" << args.session_id << "] ---\n"
                  << *res << "\n----------------------" << std::endl;
      } else {
        std::cout << "Scratchpad is empty or session not found." << std::endl;
      }
    } else if (scratch_op == "edit") {
      auto current = db_->GetScratchpad(args.session_id);
      std::string initial = current.ok() ? *current : "";
      std::string updated = TriggerEditor(initial);
      if (!updated.empty()) {
        HandleStatus(db_->UpdateScratchpad(args.session_id, updated));
        std::cout << "Scratchpad updated." << std::endl;
      } else {
        std::cout << "Scratchpad not updated (empty or editor error)." << std::endl;
      }
    } else {
      std::cout << "Unknown scratchpad operation: " << scratch_op << ". Use read or edit." << std::endl;
    }
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
    std::cout << "Usage Stats for Session [" << args.session_id << "]" << std::endl;
    HandleStatus(PrintJsonAsTable(*res));
  }

  if (orchestrator_ && orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ &&
      oauth_handler_->IsEnabled()) {
    auto token_or = oauth_handler_->GetValidToken();
    if (token_or.ok()) {
      auto quota_or = orchestrator_->GetQuota(*token_or);
      if (quota_or.ok() && quota_or->is_object()) {
        std::cout << "\n--- Gemini User Quota ---" << std::endl;
        nlohmann::json table = nlohmann::json::array();
        if (quota_or->contains("buckets") && (*quota_or)["buckets"].is_array()) {
          for (const auto& b : (*quota_or)["buckets"]) {
            if (!b.is_object()) continue;
            nlohmann::json row;
            row["Model ID"] = b.value("modelId", "N/A");
            row["Remaining Amount"] = b.value("remainingAmount", "N/A");
            row["Remaining Fraction"] = b.value("remainingFraction", 0.0);
            row["Reset Time"] = b.value("resetTime", "N/A");
            row["Token Type"] = b.value("tokenType", "N/A");
            table.push_back(row);
          }
        }
        if (!table.empty())
          HandleStatus(PrintJsonAsTable(table.dump()));
        else
          std::cout << "No quota buckets found." << std::endl;
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
  std::cout << "Executing: " << args.args << std::endl;
  int res = std::system(args.args.c_str());
  std::cout << "Exit code: " << res << std::endl;
  return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleSchema([[maybe_unused]] CommandArgs& args) {
  auto res = db_->Query("SELECT sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
  if (res.ok()) {
    auto j = nlohmann::json::parse(*res, nullptr, false);
    if (!j.is_discarded()) {
      for (const auto& row : j) {
        std::cout << row["sql"].get<std::string>() << ";\n" << std::endl;
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

CommandHandler::Result CommandHandler::HandleMemo(CommandArgs& args) {
  std::vector<std::string> parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
  std::string sub_cmd = parts[0];

  if (sub_cmd == "list") {
    auto memos_or = db_->GetAllMemos();
    if (!memos_or.ok()) {
      HandleStatus(memos_or.status(), "Error");
      return Result::HANDLED;
    }
    for (const auto& m : *memos_or) {
      std::cout << "[" << m.id << "] (" << m.semantic_tags << ") " << m.content << std::endl;
    }
  } else if (sub_cmd == "add") {
    if (parts.size() < 2) {
      std::cerr << "Usage: /memo add <tags> <content>" << std::endl;
      return Result::HANDLED;
    }
    std::vector<std::string> add_parts = absl::StrSplit(parts[1], absl::MaxSplits(' ', 1));
    if (add_parts.size() < 2) {
      std::cerr << "Usage: /memo add <tags> <content>" << std::endl;
      return Result::HANDLED;
    }
    std::string tags_str = add_parts[0];
    std::string content = add_parts[1];
    std::vector<std::string> tags = absl::StrSplit(tags_str, ',');
    for (auto& t : tags) t = std::string(absl::StripAsciiWhitespace(t));
    nlohmann::json tags_json = tags;
    HandleStatus(db_->AddMemo(content, tags_json.dump()));
    std::cout << "Memo added." << std::endl;
  } else if (sub_cmd == "search") {
    if (parts.size() < 2) {
      std::cerr << "Usage: /memo search <tags or keywords>" << std::endl;
      return Result::HANDLED;
    }
    // Try splitting by comma first, if no comma, use the whole string which will be split by ExtractTags
    std::vector<std::string> tags_input;
    if (absl::StrContains(parts[1], ',')) {
      tags_input = absl::StrSplit(parts[1], ',');
    } else {
      tags_input.push_back(parts[1]);
    }

    auto memos_or = db_->GetMemosByTags(tags_input);
    if (!memos_or.ok()) {
      HandleStatus(memos_or.status(), "Error");
      return Result::HANDLED;
    }
    if (memos_or->empty()) {
      std::cout << "No matching memos found." << std::endl;
    } else {
      for (const auto& m : *memos_or) {
        std::cout << "[" << m.id << "] (" << m.semantic_tags << ") " << m.content << std::endl;
      }
    }
  } else {
    std::cerr << "Unknown memo sub-command: " << sub_cmd << std::endl;
  }
  return Result::HANDLED;
}

std::string CommandHandler::TriggerEditor(const std::string& initial_content) {
  return slop::OpenInEditor(initial_content);
}

absl::StatusOr<std::string> CommandHandler::ExecuteCommand(const std::string& command) {
  return slop::RunCommand(command);
}

CommandHandler::Result CommandHandler::HandleManualReview(CommandArgs& args) {
  auto git_check = ExecuteCommand("git rev-parse --is-inside-work-tree");
  if (!git_check.ok() || !absl::StrContains(*git_check, "true")) {
    std::cerr << "Error: /manual-review is only available inside a git repository." << std::endl;
    return Result::HANDLED;
  }

  // Handle new files with intent-to-add
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

  auto diff_or = ExecuteCommand("git diff");
  if (!diff_or.ok() || diff_or->empty()) {
    std::cout << "No changes to review." << std::endl;
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

  std::string edited = TriggerEditor(initial_content);
  if (edited.empty() || edited == initial_content) {
    return Result::HANDLED;
  }

  // Check if any R: comments were added (at the start of a line)
  bool has_comments = false;
  std::vector<std::string> lines = absl::StrSplit(edited, '\n');
  for (const auto& line : lines) {
    std::string_view trimmed = absl::StripLeadingAsciiWhitespace(line);
    if (absl::StartsWith(trimmed, "R:")) {
      has_comments = true;
      break;
    }
  }

  if (!has_comments) {
    std::cout << "No 'R:' comments found. Ignoring review." << std::endl;
    return Result::HANDLED;
  }

  args.input = "The user has reviewed the current changes. Here is the diff with their 'R:' comments:\n\n" + edited +
               "\n\nPlease address the instructions marked with 'R:' in the diff above. Do not commit any changes "
               "after addressing.";

  return Result::PROCEED_TO_LLM;
}

}  // namespace slop
