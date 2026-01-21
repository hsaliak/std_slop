#include <cstdio>
#include "command_handler.h"
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <array>
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
    commands_["/usage"] = [this](CommandArgs& args) { return HandleStats(args); };
    commands_["/models"] = [this](CommandArgs& args) { return HandleModels(args); };
    commands_["/exec"] = [this](CommandArgs& args) { return HandleExec(args); };
    commands_["/schema"] = [this](CommandArgs& args) { return HandleSchema(args); };
    commands_["/model"] = [this](CommandArgs& args) { return HandleModel(args); };
    commands_["/throttle"] = [this](CommandArgs& args) { return HandleThrottle(args); };
    commands_["/todo"] = [this](CommandArgs& args) { return HandleTodo(args); };
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

CommandHandler::Result CommandHandler::HandleExit([[maybe_unused]] CommandArgs& args) {
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
    if (sub_cmd == "show") {
	    auto s = db_->GetContextSettings(args.session_id);
	    std::stringstream ss;
	    ss << "--- CONTEXT STATUS ---\n";
	    ss  << "Session: " << args.session_id << "\n";
	    ss << "Window Size: " ;
	    ss << (s.ok() ? (s->size == 0 ? "Infinite" : std::to_string(s->size)) : "Error");
	    ss <<  "\n";
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
        if (res.ok()) log_status(PrintJsonAsTable(*res));
    } else if (sub_cmd == "show") {
        auto res = db_->Query("SELECT name, description, json_schema FROM tools WHERE name = '" + sub_args + "'");
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
                    bool active = std::find(args.active_skills.begin(), args.active_skills.end(), row["name"].get<std::string>()) != args.active_skills.end();
                    row["status"] = active ? "ACTIVE" : "inactive";
                }
                log_status(PrintJsonAsTable(j.dump()));
            }
        }
    } else if (sub_cmd == "activate") {
        auto res = db_->Query("SELECT name FROM skills WHERE id = '" + sub_args + "' OR name = '" + sub_args + "'");
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
        auto res = db_->Query("SELECT name FROM skills WHERE id = '" + sub_args + "' OR name = '" + sub_args + "'");
        if (res.ok()) {
            auto j = nlohmann::json::parse(*res, nullptr, false);
            if (!j.is_discarded() && !j.empty()) {
                std::string name = j[0]["name"];
                args.active_skills.erase(std::remove(args.active_skills.begin(), args.active_skills.end(), name), args.active_skills.end());
                std::cout << "Skill '" << name << "' deactivated." << std::endl;
            }
        }
    } else if (sub_cmd == "show") {
        auto res = db_->Query("SELECT name, description, system_prompt_patch FROM skills WHERE name = '" + sub_args + "' OR id = '" + sub_args + "'");
        if (res.ok()) {
            auto j = nlohmann::json::parse(*res, nullptr, false);
            if (!j.is_discarded() && !j.empty()) {
                std::cout << "Skill: " << j[0]["name"].get<std::string>() << std::endl;
                std::cout << "Description: " << j[0]["description"].get<std::string>() << std::endl;
                std::cout << "Patch:\n" << j[0]["system_prompt_patch"].get<std::string>() << std::endl;
            }
        }
    } else if (sub_cmd == "delete") {
        log_status(db_->DeleteSkill(sub_args));
        std::cout << "Skill deleted." << std::endl;
    } else if (sub_cmd == "add") {
        std::string name, desc, patch;
        std::cout << "Skill Name: "; std::getline(std::cin, name);
        std::cout << "Description: "; std::getline(std::cin, desc);
        std::cout << "System Prompt Patch (Ctrl+D to finish):\n";
        std::string line;
        while (std::getline(std::cin, line)) patch += line + "\n";
        Database::Skill s{0, name, desc, patch};
        log_status(db_->RegisterSkill(s));
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleSession(CommandArgs& args) {
    std::vector<std::string> sub_parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = sub_parts[0];
    std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";

    if (sub_cmd == "list") {
        auto res = db_->Query("SELECT DISTINCT session_id FROM messages UNION SELECT DISTINCT id FROM sessions");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
    } else if (sub_cmd == "activate") {
        args.session_id = sub_args;
        std::cout << "Session switched to: " << sub_args << std::endl;
        if (orchestrator_) (void)orchestrator_->RebuildContext(args.session_id);
    } else if (sub_cmd == "remove") {
        log_status(db_->DeleteSession(sub_args));
        std::cout << "Session " << sub_args << " deleted." << std::endl;
        if (args.session_id == sub_args) {
            args.session_id = "default_session";
            std::cout << "Returning to default_session." << std::endl;
        }
    } else if (sub_cmd == "clear") {
        log_status(db_->DeleteSession(args.session_id));
        std::cout << "Session " << args.session_id << " history and state cleared." << std::endl;
        if (orchestrator_) (void)orchestrator_->RebuildContext(args.session_id);
    }
    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleStats(CommandArgs& args) {
    auto res = db_->Query(absl::Substitute(
        "SELECT model, SUM(prompt_tokens) as prompt, SUM(completion_tokens) as completion, "
        "SUM(prompt_tokens + completion_tokens) as total FROM usage "
        "WHERE session_id = '$0' GROUP BY model", args.session_id));
    if (res.ok()) {
        std::cout << "Usage Stats for Session [" << args.session_id << "]" << std::endl;
        log_status(PrintJsonAsTable(*res));
    }

    if (orchestrator_ && orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ && oauth_handler_->IsEnabled()) {
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
			    if (!table.empty()) log_status(PrintJsonAsTable(table.dump()));
			    else std::cout << "No quota buckets found." << std::endl;
		    } else {
			    std::cout << "Could not fetch quota: " << quota_or.status().message() << std::endl;
		    }
        }
    }

    return Result::HANDLED;
}

CommandHandler::Result CommandHandler::HandleModels(CommandArgs& args) {
    if (!orchestrator_) return Result::HANDLED;
    
    std::string api_key = (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI) ? google_api_key_ : openai_api_key_;
    if (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ && oauth_handler_->IsEnabled()) {
        auto token_or = oauth_handler_->GetValidToken();
        if (token_or.ok()) api_key = *token_or;
    }

    auto models_or = orchestrator_->GetModels(api_key);
    if (!models_or.ok()) {
        std::cerr << "Error fetching models: " << models_or.status().message() << std::endl;
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

CommandHandler::Result CommandHandler::HandleTodo(CommandArgs& args) {
    std::vector<std::string> parts = absl::StrSplit(args.args, absl::MaxSplits(' ', 1));
    std::string sub_cmd = parts[0];
    
    if (sub_cmd == "list") {
        std::string group = (parts.size() > 1) ? parts[1] : "";
        auto res = db_->GetTodos(group);
        if (res.ok()) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto& t : *res) {
                j.push_back({{"id", t.id}, {"group", t.group_name}, {"status", t.status}, {"description", t.description}});
            }
            log_status(PrintJsonAsTable(j.dump()));
        }
    } else if (sub_cmd == "add") {
        std::vector<std::string> add_parts = absl::StrSplit(parts[1], absl::MaxSplits(' ', 1));
        if (add_parts.size() == 2) {
            log_status(db_->AddTodo(add_parts[0], add_parts[1]));
            std::cout << "Todo added to group: " << add_parts[0] << std::endl;
        }
    } else if (sub_cmd == "edit") {
        std::vector<std::string> edit_parts = absl::StrSplit(parts[1], absl::MaxSplits(' ', 2));
        if (edit_parts.size() == 3) {
            int id = std::atoi(edit_parts[1].c_str());
            log_status(db_->UpdateTodo(id, edit_parts[0], edit_parts[2]));
            std::cout << "Todo " << id << " in group " << edit_parts[0] << " updated." << std::endl;
        }
    } else if (sub_cmd == "complete") {
        std::vector<std::string> comp_parts = absl::StrSplit(parts[1], ' ');
        if (comp_parts.size() == 2) {
            int id = std::atoi(comp_parts[1].c_str());
            log_status(db_->UpdateTodoStatus(id, comp_parts[0], "Complete"));
            std::cout << "Todo " << id << " in group " << comp_parts[0] << " marked as Complete." << std::endl;
        }
    } else if (sub_cmd == "drop") {
        log_status(db_->DeleteTodoGroup(parts[1]));
        std::cout << "All todos in group " << parts[1] << " deleted." << std::endl;
    }
    return Result::HANDLED;
}

} // namespace slop
