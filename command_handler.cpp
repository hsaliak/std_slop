#include "command_handler.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/numbers.h"
#include "absl/strings/substitute.h"
#include "orchestrator.h"
#include "oauth_handler.h"
#include "git_helper.h"

namespace slop {

CommandHandler::Result CommandHandler::Handle(std::string& input, std::string& session_id, std::vector<std::string>& active_skills, std::function<void()> show_help_fn, const std::vector<std::string>& selected_groups) {
    std::string trimmed = std::string(absl::StripLeadingAsciiWhitespace(input));
    if (trimmed.empty() || trimmed[0] != '/') return Result::NOT_A_COMMAND;

    std::vector<std::string> parts = absl::StrSplit(trimmed, absl::MaxSplits(' ', 1));
    std::string cmd = parts[0];
    std::string args = (parts.size() > 1) ? parts[1] : "";

    auto log_status = [](const absl::Status& status) {
        if (!status.ok()) std::cerr << "Error: " << status.message() << std::endl;
    };

    if (cmd == "/help") {
        show_help_fn();
        return Result::HANDLED;
    } else if (cmd == "/exit" || cmd == "/quit") {
        return Result::HANDLED;
    } else if (cmd == "/edit") {
        std::string edited = OpenInEditor();
        if (edited.empty()) return Result::HANDLED;
        input = edited;
        return Result::PROCEED_TO_LLM;
    } else if (cmd == "/message") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
        if (sub_cmd == "list") {
            int n = sub_args.empty() ? 10 : std::atoi(sub_args.c_str());
            std::string sql = absl::Substitute(
                "SELECT group_id, content as prompt FROM messages "
                "WHERE session_id = '$0' AND role = 'user' "
                "GROUP BY group_id ORDER BY created_at DESC LIMIT $1",
                session_id, n);
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
    
    } else if (cmd == "/undo") {
        if (orchestrator_) {
            auto status = orchestrator_->UndoLastGroup(session_id);
            if (status.ok()) std::cout << "Undid last interaction." << std::endl;
            else std::cerr << "Undo error: " << status.message() << std::endl;
        } else {
            std::cerr << "Orchestrator not available for undo." << std::endl;
        }
        return Result::HANDLED;

    } else if (cmd == "/window") {
        int n = args.empty() ? 0 : std::atoi(args.c_str());
        log_status(db_->SetContextWindow(session_id, n));
        if (n > 0) std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
        else if (n == 0) std::cout << "Full Context Mode (infinite buffer)." << std::endl;
        else std::cout << "Context Hidden (None)." << std::endl;
        return Result::HANDLED;
    } else if (cmd == "/context") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
        if (sub_cmd == "window") {
            int n = sub_args.empty() ? 0 : std::atoi(sub_args.c_str());
            log_status(db_->SetContextWindow(session_id, n));
            if (n > 0) std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
            else if (n == 0) std::cout << "Full Context Mode (infinite buffer)." << std::endl;
            else std::cout << "Context Hidden (None)." << std::endl;
        } else if (sub_cmd == "rebuild") {
            if (orchestrator_) {
                auto status = orchestrator_->RebuildContext(session_id); if (status.ok()) std::cout << "Context rebuilt from history." << std::endl; else std::cerr << "Error: " << status.message() << std::endl;
            } else {
                std::cerr << "Orchestrator not available for rebuilding context." << std::endl;
            }
        } else if (sub_cmd == "show") {
            if (orchestrator_) {
                auto prompt_or = orchestrator_->AssemblePrompt(session_id, active_skills);
                if (prompt_or.ok()) {
                    DisplayAssembledContext(prompt_or->dump());
                } else {
                    std::cerr << "Error assembling prompt: " << prompt_or.status().message() << std::endl;
                    log_status(DisplayHistory(*db_, session_id, 20, selected_groups));
                }
            } else {
                log_status(DisplayHistory(*db_, session_id, 20, selected_groups));
            }
        } else {
            auto s = db_->GetContextSettings(session_id);
            if (s.ok()) {
                if (s->size > 0) std::cout << "Current Context: Rolling window of last " << s->size << " groups." << std::endl;
                else if (s->size == 0) std::cout << "Current Context: Full history." << std::endl;
                else std::cout << "Current Context: None (Hidden)." << std::endl;
            }
            if (orchestrator_) {
                auto prompt_or = orchestrator_->AssemblePrompt(session_id, active_skills);
                if (prompt_or.ok()) {
                    DisplayAssembledContext(prompt_or->dump());
                }
            }
            std::cout << "Usage: /context [window <N>|rebuild|show]" << std::endl;
        }
        return Result::HANDLED;
    
    } else if (cmd == "/tool") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
        if (sub_cmd == "list") {
            auto t_or = db_->GetEnabledTools();
            if (t_or.ok()) {
                for (const auto& t : *t_or) {
                    std::cout << "- " << t.name << ": " << t.description << std::endl;
                }
            }
        } else if (sub_cmd == "show") {
            auto t_or = db_->GetEnabledTools();
            if (t_or.ok()) {
                for (const auto& t : *t_or) {
                    if (t.name == sub_args) {
                        std::string out = "Name: " + t.name + "\nDescription: " + t.description + "\nSchema: " + t.json_schema;
                        SmartDisplay(out);
                        break;
                    }
                }
            }
        }
        return Result::HANDLED;

    } else if (cmd == "/prompt-ledger") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(" ", 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
        if (sub_cmd == "on") {
            if (!GitHelper::IsGitRepo()) {
                std::cout << "Not a git repository. Initialize? [y/N] ";
                std::string resp; std::getline(std::cin, resp);
                if (resp == "y" || resp == "Y") log_status(GitHelper::InitRepo());
                else { std::cout << "Prompt ledger requires a git repository." << std::endl; return Result::HANDLED; }
            }
            log_status(db_->SetGlobalSetting("prompt_ledger_enabled", "true"));
            std::cout << "Prompt ledger enabled." << std::endl;
        } else if (sub_cmd == "off") {
            log_status(db_->SetGlobalSetting("prompt_ledger_enabled", "false"));
            std::cout << "Prompt ledger disabled." << std::endl;
        } else if (sub_cmd == "show" || (!sub_cmd.empty() && sub_cmd != "on" && sub_cmd != "off")) {
            std::string gid = (sub_cmd == "show") ? sub_args : sub_cmd;
            auto diff_or = GitHelper::GetDiff(gid);
            if (diff_or.ok()) SmartDisplay(*diff_or);
            else std::cerr << "Error: " << diff_or.status().message() << std::endl;
        } else {
            auto val = db_->GetGlobalSetting("prompt_ledger_enabled");
            bool enabled = val.ok() && *val == "true";
            std::cout << "Prompt Ledger: " << (enabled ? "ON" : "OFF") << std::endl;
            std::cout << "Usage: /prompt-ledger [on|off|show <group_id>]" << std::endl;
            std::cout << "       /prompt-ledger <group_id>" << std::endl;
        }
        return Result::HANDLED;

    } else if (cmd == "/skill") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";

        if (sub_cmd == "activate") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill activate <name|id>" << std::endl;
            } else {
                active_skills.push_back(sub_args);
                std::cout << "Skill activated: " << sub_args << std::endl;
            }
        } else if (sub_cmd == "deactivate") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill deactivate <name|id>" << std::endl;
            } else {
                auto it = std::find(active_skills.begin(), active_skills.end(), sub_args);
                if (it != active_skills.end()) {
                    active_skills.erase(it);
                    std::cout << "Skill deactivated: " << sub_args << std::endl;
                }
            }
        } else if (sub_cmd == "list") {
            auto all_skills_or = db_->GetSkills();
            if (all_skills_or.ok()) {
                std::cout << "--- Available Skills ---" << std::endl;
                for (const auto& skill : *all_skills_or) {
                    bool active = std::find(active_skills.begin(), active_skills.end(), skill.name) != active_skills.end();
                    std::cout << (active ? "[*] " : "[ ] ") << skill.id << ": " << skill.name << " - " << skill.description << std::endl;
                }
            }
        } else if (sub_cmd == "add") {
             std::string name, desc, patch;
             std::cout << "Skill Name: "; std::getline(std::cin, name);
             std::cout << "Description: "; std::getline(std::cin, desc);
             std::cout << "System Prompt Patch (Type END on a new line to finish):\n";
             std::string line;
             while (std::getline(std::cin, line) && line != "END") patch += line + "\n";
             Database::Skill s = {0, name, desc, patch};
             log_status(db_->RegisterSkill(s));
        } else if (sub_cmd == "delete") {
             log_status(db_->DeleteSkill(sub_args));
             std::cout << "Skill deleted." << std::endl;
        } else {
            std::cout << "Usage: /skill [list|activate|deactivate|add|delete]" << std::endl;
        }
        return Result::HANDLED;

    } else if (cmd == "/session") {
        if (args.empty()) {
            auto res = db_->Query("SELECT id FROM sessions");
            if (res.ok()) log_status(PrintJsonAsTable(*res));
        } else {
            std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
            if (sub_parts[0] == "remove") {
                if (sub_parts.size() > 1) {
                    log_status(db_->DeleteSession(sub_parts[1]));
                    std::cout << "Session " << sub_parts[1] << " removed." << std::endl;
                }
            } else {
                session_id = args;
                std::cout << "Switched to session: " << session_id << std::endl;
                if (orchestrator_) orchestrator_->RebuildContext(session_id);
            }
        }
        return Result::HANDLED;

    } else if (cmd == "/stats" || cmd == "/usage") {
        auto res = db_->GetTotalUsage(session_id);
        if (res.ok()) {
            std::cout << "--- Session Usage Stats ---" << std::endl;
            std::cout << "Prompt Tokens:     " << res->prompt_tokens << std::endl;
            std::cout << "Completion Tokens: " << res->completion_tokens << std::endl;
            std::cout << "Total Tokens:      " << res->total_tokens << std::endl;
        }
        return Result::HANDLED;

    } else if (cmd == "/schema") {
        auto res = db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
        return Result::HANDLED;

    } else if (cmd == "/models") {
        if (orchestrator_) {
            std::string key;
            if (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI) {
                if (oauth_handler_ && oauth_handler_->IsEnabled()) {
                    auto token_or = oauth_handler_->GetValidToken();
                    if (token_or.ok()) key = *token_or;
                } else {
                    key = google_api_key_;
                }
            } else {
                key = openai_api_key_;
            }
            auto m_or = orchestrator_->GetModels(key);
            if (m_or.ok()) {
                for (const auto& m : *m_or) std::cout << "- " << m << std::endl;
            }
        }
        return Result::HANDLED;

    } else if (cmd == "/exec") {
        std::string full_cmd = args + " | ${PAGER:-less -F -X}";
        (void)std::system(full_cmd.c_str());
        return Result::HANDLED;

    } else if (cmd == "/model") {
        if (orchestrator_) {
            if (args.empty()) {
                std::cout << "Current model: " << orchestrator_->GetModel() << std::endl;
            } else {
                orchestrator_->SetModel(std::string(absl::StripAsciiWhitespace(args)));
                std::cout << "Model changed to: " << orchestrator_->GetModel() << std::endl;
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/throttle") {
        if (orchestrator_) {
            if (args.empty()) {
                std::cout << "Current throttle: " << orchestrator_->GetThrottle() << " seconds" << std::endl;
            } else {
                int seconds = 0;
                if (absl::SimpleAtoi(args, &seconds)) {
                    orchestrator_->SetThrottle(seconds);
                    std::cout << "Throttle set to: " << seconds << " seconds" << std::endl;
                }
            }
        }
        return Result::HANDLED;
    }

    return Result::NOT_A_COMMAND;
}

} // namespace slop
