#include "command_handler.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/numbers.h"
#include "orchestrator.h"
#include "oauth_handler.h"

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
            auto res = db_->Query("SELECT group_id, role, substr(content, 1, 50) as preview, status FROM messages WHERE session_id = '" + session_id + "' ORDER BY created_at DESC LIMIT " + std::to_string(n));
            if (res.ok()) log_status(PrintJsonAsTable(*res));
        } else if (sub_cmd == "view") {
            auto res = db_->Query("SELECT role, content FROM messages WHERE group_id = '" + sub_args + "' ORDER BY created_at ASC");
            if (res.ok()) {
                auto j = nlohmann::json::parse(*res, nullptr, false);
                if (!j.is_discarded() && !j.empty()) {
                    std::string out;
                    for (const auto& m : j) out += "[" + m["role"].get<std::string>() + "]:\n" + m["content"].get<std::string>() + "\n\n";
                    OpenInEditor(out);
                }
            }
        } else if (sub_cmd == "remove") {
            log_status(db_->Execute("DELETE FROM messages WHERE group_id = '" + sub_args + "'"));
        } else if (sub_cmd == "drop") {
            log_status(db_->Execute("UPDATE messages SET status = 'dropped' WHERE group_id = '" + sub_args + "'"));
        }
        return Result::HANDLED;
    } else if (cmd == "/context") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";
        if (sub_cmd == "full") {
            int n = sub_args.empty() ? 0 : std::atoi(sub_args.c_str());
            log_status(db_->SetContextMode(session_id, Database::ContextMode::FULL, n));
            if (n > 0) std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
            else std::cout << "Full Context Mode (infinite buffer)." << std::endl;
        } else if (sub_cmd == "drop") {
            log_status(db_->Execute("UPDATE messages SET status = 'dropped' WHERE session_id = '" + session_id + "'"));
        } else if (sub_cmd == "build") {
            auto settings = db_->GetContextSettings(session_id);
            if (settings.ok() && settings->mode == Database::ContextMode::FTS_RANKED) {
                std::cout << "[1;33m[Warning] Session is in FTS mode. Manual context building is ignored in this mode.[0m" << std::endl;
                std::cout << "Context will be dynamically retrieved based on relevance to your next query." << std::endl;
                std::cout << "Switch to full mode using: /context-mode full" << std::endl;
            }
            int n = sub_args.empty() ? 5 : std::atoi(sub_args.c_str());
            std::string sub = "SELECT DISTINCT group_id FROM messages WHERE session_id = '" + session_id + "' ORDER BY created_at DESC LIMIT " + std::to_string(n);
            log_status(db_->Execute("UPDATE messages SET status = 'completed' WHERE group_id IN (" + sub + ")"));
        } else if (sub_cmd == "show") {
            auto settings = db_->GetContextSettings(session_id);
            if (settings.ok() && settings->mode == Database::ContextMode::FTS_RANKED) {
                std::cout << "\033[1;36m[Note] Session is in FTS (Ranked) mode. Context is generated dynamically per-query.\033[0m" << std::endl;
                std::cout << "Showing the assembled prompt based on the most recent user query (if any):" << std::endl;
            }
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
        }
        return Result::HANDLED;
    } else if (cmd == "/context-mode") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        if (sub_parts[0] == "fts") {
            int n = (sub_parts.size() > 1) ? std::atoi(sub_parts[1].c_str()) : 5;
            if (n < 1) n = 1;
            log_status(db_->SetContextMode(session_id, Database::ContextMode::FTS_RANKED, n));
            std::cout << "FTS-Ranked Context Mode (Size: " << n << " groups)" << std::endl;
        } else if (sub_parts[0] == "full") {
            int n = (sub_parts.size() > 1) ? std::atoi(sub_parts[1].c_str()) : 0;
            log_status(db_->SetContextMode(session_id, Database::ContextMode::FULL, n));
            if (n > 0) std::cout << "Rolling Window Context: Last " << n << " interaction groups." << std::endl;
            else std::cout << "Full Context Mode (infinite buffer)." << std::endl;
        }
        else {
            auto s = db_->GetContextSettings(session_id);
            if (s.ok()) std::cout << "Current Mode: " << (s->mode == Database::ContextMode::FTS_RANKED ? "FTS_RANKED" : "FULL") << " (Size: " << s->size << ")" << std::endl;
            std::cout << "Usage: /context-mode fts <N> | full" << std::endl;
        }
        return Result::HANDLED;
    } else if (cmd == "/skill") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        std::string sub_cmd = sub_parts[0];
        std::string sub_args = (sub_parts.size() > 1) ? sub_parts[1] : "";

        if (sub_cmd == "activate") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill activate <name|id>" << std::endl;
                return Result::HANDLED;
            }
            auto s_or = db_->GetSkills();
            if (s_or.ok()) {
                std::string target = std::string(absl::StripAsciiWhitespace(sub_args));
                for (const auto& s : *s_or) {
                    bool match = false;
                    if (s.name == target) match = true;
                    else {
                        int id;
                        if (absl::SimpleAtoi(target, &id) && s.id == id) {
                            match = true;
                        }
                    }
                    if (match) {
                        bool already_active = false;
                        for (const auto& a : active_skills) if (a == s.name) already_active = true;
                        if (!already_active) {
                            active_skills.push_back(s.name);
                            std::cout << "Activated: " << s.name << std::endl;
                        } else {
                            std::cout << "Skill '" << s.name << "' is already active." << std::endl;
                        }
                        return Result::HANDLED;
                    }
                }
            }
            std::cout << "Skill not found: " << sub_args << std::endl;
        } else if (sub_cmd == "deactivate") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill deactivate <name|id>" << std::endl;
                return Result::HANDLED;
            }
            std::string target = std::string(absl::StripAsciiWhitespace(sub_args));
            std::string actual_name;
            
            // Resolve name if ID provided
            int id;
            if (absl::SimpleAtoi(target, &id)) {
                auto s_or = db_->GetSkills();
                if (s_or.ok()) {
                    for (const auto& s : *s_or) if (s.id == id) actual_name = s.name;
                }
            } else {
                actual_name = target;
            }

            auto it = std::find(active_skills.begin(), active_skills.end(), actual_name);
            if (it != active_skills.end()) {
                active_skills.erase(it);
                std::cout << "Deactivated: " << actual_name << std::endl;
            } else {
                std::cout << "Skill '" << target << "' was not active." << std::endl;
            }
        } else if (sub_cmd == "list") {
            auto s_or = db_->GetSkills();
            if (s_or.ok()) {
                for (const auto& s : *s_or) {
                    bool active = false;
                    for (const auto& a : active_skills) if (a == s.name) active = true;
                    std::cout << (active ? "* " : "  ") << "[" << s.id << "] " << s.name << " - " << s.description << std::endl;
                }
            }
        } else if (sub_cmd == "add" || sub_cmd == "edit") {
            std::string template_str = "#name: \n#description: \n#patch: \n";
            Database::Skill existing;
            bool is_edit = (sub_cmd == "edit");
            if (is_edit) {
                if (sub_args.empty()) {
                    std::cout << "Usage: /skill edit <name|id>" << std::endl;
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
                        if (match) { existing = s; break; }
                    }
                }
                if (existing.name.empty()) {
                    std::cout << "Skill not found." << std::endl;
                    return Result::HANDLED;
                }
                template_str = "#name: " + existing.name + "\n#description: " + existing.description + "\n#patch: " + existing.system_prompt_patch + "\n";
            }

            std::string edited = OpenInEditor(template_str);
            if (edited.empty() || edited == template_str) return Result::HANDLED;

            Database::Skill news;
            std::vector<std::string> lines = absl::StrSplit(edited, '\n');
            std::string current_field;
            for (const auto& line : lines) {
                if (absl::StartsWith(line, "#name: ")) { news.name = absl::StripAsciiWhitespace(line.substr(7)); current_field = "name"; }
                else if (absl::StartsWith(line, "#description: ")) { news.description = absl::StripAsciiWhitespace(line.substr(14)); current_field = "description"; }
                else if (absl::StartsWith(line, "#patch: ")) { news.system_prompt_patch = line.substr(8) + "\n"; current_field = "patch"; }
                else if (!current_field.empty()) {
                    if (current_field == "patch") news.system_prompt_patch += line + "\n";
                }
            }
            if (news.name.empty()) {
                std::cout << "Skill name cannot be empty." << std::endl;
                return Result::HANDLED;
            }
            
            log_status(db_->RegisterSkill(news));
            std::cout << "Skill '" << news.name << "' saved." << std::endl;
        } else if (sub_cmd == "view") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill view <name|id>" << std::endl;
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
                        std::cout << "ID: " << s.id << "\nName: " << s.name << "\nDescription: " << s.description << "\nPatch:\n" << s.system_prompt_patch << std::endl;
                        return Result::HANDLED;
                    }
                }
            }
            std::cout << "Skill not found." << std::endl;
        } else if (sub_cmd == "delete") {
            if (sub_args.empty()) {
                std::cout << "Usage: /skill delete <name|id>" << std::endl;
                return Result::HANDLED;
            }
            std::string target = std::string(absl::StripAsciiWhitespace(sub_args));
            std::string actual_name;
            
            // Resolve name if ID provided
            int id;
            if (absl::SimpleAtoi(target, &id)) {
                auto s_or = db_->GetSkills();
                if (s_or.ok()) {
                    for (const auto& s : *s_or) if (s.id == id) actual_name = s.name;
                }
            } else {
                actual_name = target;
            }

            if (!actual_name.empty()) {
                auto it = std::remove(active_skills.begin(), active_skills.end(), actual_name);
                active_skills.erase(it, active_skills.end());
            }
            log_status(db_->DeleteSkill(target));
            std::cout << "Skill deleted." << std::endl;
        } else {
            std::cout << "Unknown skill subcommand: " << sub_cmd << "\nUsage: /skill [activate|add|edit|view|delete]" << std::endl;
        }
        return Result::HANDLED;
    } else if (cmd == "/sessions") {
        auto res = db_->Query("SELECT DISTINCT session_id FROM messages");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
        return Result::HANDLED;
    } else if (cmd == "/switch") {
        if (!args.empty()) { 
            session_id = args; 
            std::cout << "Switched to: " << session_id << std::endl; 
            log_status(DisplayHistory(*db_, session_id, 20, selected_groups));
        }
        return Result::HANDLED;
    } else if (cmd == "/undo") {
        auto gid_res = db_->Query("SELECT group_id FROM messages WHERE session_id = '" + session_id + "' AND status != 'dropped' ORDER BY created_at DESC LIMIT 1");
        if (gid_res.ok()) {
            auto j = nlohmann::json::parse(*gid_res, nullptr, false);
            if (!j.is_discarded() && !j.empty()) {
                log_status(db_->Execute("UPDATE messages SET status = 'dropped' WHERE group_id = '" + j[0]["group_id"].get<std::string>() + "'"));
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/stats") {
        auto res = db_->Query("SELECT role, count(*) as count, status FROM messages WHERE session_id = '" + session_id + "' GROUP BY role, status");
        if (res.ok()) {
            std::cout << "--- Session Message Stats ---" << std::endl;
            log_status(PrintJsonAsTable(*res));
        }

        if (orchestrator_ && orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI && oauth_handler_ && oauth_handler_->IsEnabled()) {
            auto token_or = oauth_handler_->GetValidToken();
            if (token_or.ok()) {
                auto quota_or = orchestrator_->GetQuota(*token_or);
                if (quota_or.ok()) {
                    std::cout << "\n--- Gemini User Quota ---" << std::endl;
                    nlohmann::json table = nlohmann::json::array();
                    if (quota_or->contains("buckets")) {
                        for (const auto& b : (*quota_or)["buckets"]) {
                            nlohmann::json row;
                            row["Model ID"] = b.value("modelId", "N/A");
                            row["Remaining Amount"] = b.value("remainingAmount", "N/A");
                            row["Remaining Fraction"] = b.value("remainingFraction", 0.0);
                            row["Reset Time"] = b.value("resetTime", "N/A");
                            row["Token Type"] = b.value("tokenType", "N/A");
                            table.push_back(row);
                        }
                    }
                    if (!table.empty()) {
                        log_status(PrintJsonAsTable(table.dump()));
                    } else {
                        std::cout << "No quota buckets found." << std::endl;
                    }
                } else {
                    std::cout << "Could not fetch quota: " << quota_or.status().message() << std::endl;
                }
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/models") {
        if (orchestrator_) {
            std::string key;
            if (orchestrator_->GetProvider() == Orchestrator::Provider::GEMINI) {
                if (oauth_handler_ && oauth_handler_->IsEnabled()) {
                    auto token_or = oauth_handler_->GetValidToken();
                    if (token_or.ok()) key = *token_or;
                } else {
                    if (!google_api_key_.empty()) key = google_api_key_;
                    else {
                        const char* env_key = std::getenv("GOOGLE_API_KEY");
                        if (env_key) key = env_key;
                    }
                }
            } else {
                if (!openai_api_key_.empty()) key = openai_api_key_;
                else {
                    const char* env_key = std::getenv("OPENAI_API_KEY");
                    if (env_key) key = env_key;
                }
            }
            auto m_or = orchestrator_->GetModels(key);
            if (m_or.ok()) {
                nlohmann::json table = nlohmann::json::array();
                for (const auto& m : *m_or) {
                    table.push_back({{"Model Name", m}});
                }
                log_status(PrintJsonAsTable(table.dump()));
            } else {
                std::cout << "Could not fetch models: " << m_or.status().message() << std::endl;
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/exec") {
        if (args.empty()) {
            std::cout << "Usage: /exec <command>" << std::endl;
            return Result::HANDLED;
        }
        std::string full_cmd = args + " | ${PAGER:-less -F -X}";
        (void)std::system(full_cmd.c_str());
        return Result::HANDLED;
    } else if (cmd == "/schema") {
        auto res = db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
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
                } else {
                    std::cout << "Invalid throttle value. Use an integer." << std::endl;
                }
            }
        }
        return Result::HANDLED;
    }
    std::cout << "Unknown command: " << cmd << std::endl;
    return Result::UNKNOWN;
}

}  // namespace slop
