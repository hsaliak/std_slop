#include <cstdio>
#include "command_handler.h"
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/numbers.h"
#include "absl/strings/substitute.h"
#include "orchestrator.h"
#include "oauth_handler.h"
#include "ui.h"
#include <unistd.h>

namespace slop {

namespace {
void log_status(const absl::Status& status) {
    if (!status.ok()) std::cerr << "Error: " << status.message() << std::endl;
}
} // namespace

CommandHandler::CommandHandler(Database* db, 
                               Orchestrator* orchestrator,
                               OAuthHandler* oauth_handler,
                               std::string google_api_key,
                               std::string openai_api_key) 
    : db_(db), orchestrator_(orchestrator), oauth_handler_(oauth_handler), 
      google_api_key_(std::move(google_api_key)), openai_api_key_(std::move(openai_api_key)) {
    RegisterCommands();
}

void CommandHandler::RegisterCommands() {
    commands_["/help"] = [this](CommandArgs& args) { return HandleHelp(args); };
    commands_["/exit"] = [this](CommandArgs& args) { return HandleExit(args); };
    commands_["/quit"] = [this](CommandArgs& args) { return HandleExit(args); };
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
}

CommandHandler::Result CommandHandler::Handle(std::string& input, std::string& session_id, std::vector<std::string>& active_skills, std::function<void()> show_help_fn, const std::vector<std::string>& selected_groups) {
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

CommandHandler::Result CommandHandler::HandleExit(CommandArgs& args) {
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleEdit(CommandArgs& args) {
    std::string edited = slop::OpenInEditor();
    if (edited.empty()) return Result::HANDLED;
    args.input = edited;
    return Result::PROCEED_TO_LLM;
}

CommandHandler::Result CommandHandler::HandleMessage(CommandArgs& args) {
    std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = sub_parts[0];
    std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
    if (sub_cmd == "list") {
        int n = sub_args.empty() ? 10 : std::atoi(sub_args.c_str());
        std::string sql = absl::Substitute(
            "SELECT group_id, content as prompt FROM messages "
            "WHERE session_id = '$0' AND role = 'user' "
            "GROUP BY group_id ORDER BY created_at DESC LIMIT $1",
            args.session_id, n);
        auto res = db_->Query(sql);
        if (res.ok()) log_status(PrintJsonAsTable(*res));
    } else if (sub_cmd == "view" || sub_cmd == "show") {
        auto res = db_->Query("SELECT role, content FROM messages WHERE group_id = '" + sub_args + "' ORDER BY created_at ASC");
        if (res.ok()) {
            auto j = nlohmann::json::parse(*res, nullptr, false);
            if (!j.is_discarded() && !j.empty()) {
                std::string out;
                for (const auto& m : j) out += "[" + m["role"].get<std::string>() + "]:\n" + m["content"].get<std::string>() + "\n\n";
                SmartDisplay(out);
            }
        }
    } else if (sub_cmd == "remove") {
        log_status(db_->Execute("DELETE FROM messages WHERE group_id = '" + sub_args + "'"));
        std::cout << "Message group " << sub_args << " deleted." << std::endl;
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleUndo(CommandArgs& args) {
    auto gid_or = db_->GetLastGroupId(args.session_id);
    if (gid_or.ok()) {
        std::string gid = *gid_or;
        log_status(db_->Execute("DELETE FROM messages WHERE group_id = '" + gid + "'"));
        std::cout << "Undid last interaction (Group ID: " + gid + ")" << std::endl;
        if (orchestrator_) {
            auto status = orchestrator_->RebuildContext(args.session_id);
            if (status.ok()) std::cout << "Context rebuilt." << std::endl;
            else std::cerr << "Error rebuilding context: " << status.message() << std::endl;
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
        log_status(db_->SetContextWindow(args.session_id, n));
        if (n > 0) std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
        else if (n == 0) std::cout << "Full Context Mode (infinite buffer)." << std::endl;
        else std::cout << "Context Hidden (None)." << std::endl;
        return Result::HANDLED;
    } 
    
    if (sub_cmd == "rebuild") {
        if (orchestrator_) {
            auto status = orchestrator_->RebuildContext(args.session_id); 
            if (status.ok()) std::cout << "Context rebuilt from history." << std::endl; 
            else std::cerr << "Error: " << status.message() << std::endl;
        } else {
            std::cerr << "Orchestrator not available for rebuilding context." << std::endl;
        }
        return Result::HANDLED;
    }

    if (sub_cmd != "show") {
        if (!sub_cmd.empty()) {
            std::cerr << "Unknown context command: " << sub_cmd << std::endl;
        }
        std::cout << "Usage: /context show | window <N> | rebuild" << std::endl;
        return Result::HANDLED;
    }

    auto s = db_->GetContextSettings(args.session_id);
    if (s.ok()) {
        std::cout << "--- CONTEXT STATUS ---" << std::endl;
        std::cout << "Session: " << args.session_id << std::endl;
        std::cout << "Window Size: " << (s->size == 0 ? "Infinite" : std::to_string(s->size)) << std::endl;
        if (!args.active_skills.empty()) {
            std::cout << "Active Skills: " << absl::StrJoin(args.active_skills, ", ") << std::endl;
        }
    }

    if (orchestrator_) {
        auto prompt_or = orchestrator_->AssemblePrompt(args.session_id, args.active_skills);
        if (prompt_or.ok()) {
            std::cout << "\n--- ASSEMBLED PROMPT (DEBUG) ---" << std::endl;
            SmartDisplay(prompt_or->dump(2));
        }
    }
    
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleTool(CommandArgs& args) {
    std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = sub_parts[0];
    std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
    if (sub_cmd == "list") {
        auto tools = db_->GetEnabledTools();
        if (tools.ok()) {
            std::cout << "Available Tools:" << std::endl;
            for (const auto& t : *tools) {
                std::cout << " - " << t.name << ": " << t.description << std::endl;
            }
        }
    } else if (sub_cmd == "show") {
        auto tools = db_->GetEnabledTools();
        bool found = false;
        if (tools.ok()) {
            for (const auto& t : *tools) {
                if (t.name == sub_args) {
                    std::cout << "Tool: " << t.name << "\nDescription: " << t.description << "\nSchema:\n" << t.json_schema << std::endl;
                    found = true;
                    break;
                }
            }
        }
        if (!found) std::cerr << "Tool not found or not enabled." << std::endl;
    } else {
        std::cout << "Usage: /tool list | show <name>" << std::endl;
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleSkill(CommandArgs& args) {
    std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = sub_parts[0];
    std::string sub_args = std::string(absl::StripAsciiWhitespace((sub_parts.size() > 1) ? sub_parts[1] : ""));

    if (sub_cmd == "activate") {
        if (sub_args.empty()) {
            std::cerr << "Usage: /skill activate <name|id>" << std::endl;
            return Result::HANDLED;
        }
        auto s_or = db_->GetSkills();
        if (s_or.ok()) {
            for (const auto& s : *s_or) {
                bool match = false;
                if (s.name == sub_args) match = true;
                else {
                    int id;
                    if (absl::SimpleAtoi(sub_args, &id) && s.id == id) match = true;
                }
                if (match) {
                    bool already_active = false;
                    for (const auto& a : args.active_skills) if (a == s.name) already_active = true;
                    if (!already_active) {
                        args.active_skills.push_back(s.name);
                        std::cout << "Skill activated: " << s.name << std::endl;
                    } else {
                        std::cout << "Skill already active: " << s.name << std::endl;
                    }
                    return Result::HANDLED;
                }
            }
            std::cerr << "Skill not found: " << sub_args << std::endl;
        }
    } else if (sub_cmd == "deactivate") {
        auto it = std::find_if(args.active_skills.begin(), args.active_skills.end(), [&](const std::string& name) {
            if (name == sub_args) return true;
            auto s_or = db_->GetSkills();
            if (s_or.ok()) {
                for (const auto& s : *s_or) {
                    int id;
                    if (s.name == name && absl::SimpleAtoi(sub_args, &id) && s.id == id) return true;
                }
            }
            return false;
        });
        if (it != args.active_skills.end()) {
            std::cout << "Skill deactivated: " << *it << std::endl;
            args.active_skills.erase(it);
        } else {
            std::cerr << "Skill not active: " << sub_args << std::endl;
        }
    } else if (sub_cmd == "list") {
        auto s_or = db_->GetSkills();
        if (s_or.ok()) {
            std::cout << "Available Skills:" << std::endl;
            for (const auto& s : *s_or) {
                bool active = false;
                for (const auto& a : args.active_skills) if (a == s.name) active = true;
                std::cout << " [" << (active ? "ACTIVE" : "      ") << "] " << s.id << ": " << s.name << " - " << s.description << std::endl;
            }
        }
    } else if (sub_cmd == "add") {
        std::string name, desc, patch;
        std::cout << "Skill Name: "; std::getline(std::cin, name);
        std::cout << "Description: "; std::getline(std::cin, desc);
        std::cout << "System Prompt Patch (Opening Editor...)" << std::endl;
        patch = slop::OpenInEditor();
        if (!name.empty()) {
            Database::Skill s = {0, name, desc, patch};
            log_status(db_->RegisterSkill(s));
            std::cout << "Skill registered." << std::endl;
        }
    } else if (sub_cmd == "edit") {
        if (sub_args.empty()) {
            std::cerr << "Usage: /skill edit <name|id>" << std::endl;
            return Result::HANDLED;
        }
        auto s_or = db_->GetSkills();
        if (s_or.ok()) {
            for (const auto& s : *s_or) {
                bool match = false;
                if (s.name == sub_args) match = true;
                else {
                    int id;
                    if (absl::SimpleAtoi(sub_args, &id) && s.id == id) match = true;
                }
                if (match) {
                    std::string new_patch = slop::OpenInEditor(s.system_prompt_patch);
                    Database::Skill updated = s;
                    updated.system_prompt_patch = new_patch;
                    log_status(db_->UpdateSkill(updated));
                    std::cout << "Skill '" << s.name << "' updated." << std::endl;
                    return Result::HANDLED;
                }
            }
            std::cerr << "Skill not found: " << sub_args << std::endl;
        }
    } else if (sub_cmd == "delete") {
        log_status(db_->Execute("DELETE FROM skills WHERE name = '" + sub_args + "' OR id = '" + sub_args + "'"));
        std::cout << "Skill deleted." << std::endl;
    } else {
        std::cout << "Usage: /skill list | activate | deactivate | add | edit | delete" << std::endl;
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleSession(CommandArgs& args) {
    std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = sub_parts[0];
    std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";

    if (sub_cmd == "activate") {
        if (sub_args.empty()) {
            std::cerr << "Usage: /session activate <name>" << std::endl;
            return Result::HANDLED;
        }
        args.session_id = sub_args;
        std::cout << "Switched to session: " << args.session_id << std::endl;
        if (orchestrator_) {
            (void)orchestrator_->RebuildContext(args.session_id);
        }
    } else if (sub_cmd == "list") {
        auto res = db_->Query("SELECT DISTINCT session_id FROM messages UNION SELECT DISTINCT id FROM sessions");
        log_status(res.status()); if (res.ok()) log_status(PrintJsonAsTable(*res));
    } else if (sub_cmd == "remove") {
        if (sub_args.empty()) {
            std::cerr << "Usage: /session remove <name>" << std::endl;
            return Result::HANDLED;
        }
        log_status(db_->DeleteSession(sub_args));
        if (args.session_id == sub_args) {
            args.session_id = "default_session";
            std::cout << "Removed current session. Switched to 'default_session'." << std::endl;
        } else {
            std::cout << "Session '" << sub_args << "' removed." << std::endl;
        }
    } else {
        std::cout << "Usage: /session list | activate <name> | remove <name>" << std::endl;
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleStats(CommandArgs& args) {
    auto res = db_->Query(absl::Substitute(
        "SELECT model, SUM(prompt_tokens) as prompt, SUM(completion_tokens) as completion, "
        "SUM(prompt_tokens + completion_tokens) as total FROM usage "
        "WHERE session_id = '$0' GROUP BY model", args.session_id));
    if (res.ok()) log_status(PrintJsonAsTable(*res));
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleModels(CommandArgs& args) {
    std::string api_key = (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI) ? google_api_key_ : openai_api_key_;
    auto models_or = orchestrator_->GetModels(api_key);
    if (models_or.ok()) {
        std::cout << "Available Models:" << std::endl;
        for (const auto& m : *models_or) {
            if (args.args.empty() || absl::StrContainsIgnoreCase(m.id, args.args) || absl::StrContainsIgnoreCase(m.name, args.args)) {
                std::cout << " - " << m.id << " (" << m.name << ")" << std::endl;
            }
        }
    } else {
        std::cerr << "Error fetching models: " << models_or.status().message() << std::endl;
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

CommandHandler::Result CommandHandler::HandleSchema(CommandArgs& args) {
    auto res = db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
    if (res.ok()) log_status(PrintJsonAsTable(*res));
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleModel(CommandArgs& args) {
    if (args.args.empty()) {
        std::cout << "Current model: " << orchestrator_->GetModel() << std::endl;
    } else {
        orchestrator_->SetModel(args.args);
        std::cout << "Model set to: " << args.args << std::endl;
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleThrottle(CommandArgs& args) {
    if (args.args.empty()) {
        std::cout << "Current throttle: " << orchestrator_->GetThrottle() << " seconds" << std::endl;
    } else {
        int t = std::atoi(args.args.c_str());
        orchestrator_->SetThrottle(t);
        std::cout << "Throttle set to " << t << " seconds." << std::endl;
    }
    return Result::HANDLED;
}

} // namespace slop
