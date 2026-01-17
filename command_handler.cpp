#include <cstdio>
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
            // Group by group_id and show the first 'user' message as the prompt
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
            // Hard delete the message group
            log_status(db_->Execute("DELETE FROM messages WHERE group_id = '" + sub_args + "'"));
            std::cout << "Message group " << sub_args << " deleted." << std::endl;
        }
        return Result::HANDLED;
    
    } else if (cmd == "/undo") {
        auto gid_or = db_->GetLastGroupId(session_id);
        if (gid_or.ok()) {
            std::string gid = *gid_or;
            log_status(db_->Execute("DELETE FROM messages WHERE group_id = '" + gid + "'"));
            std::cout << "Undid last interaction (Group ID: " + gid + ")" << std::endl;
            if (orchestrator_) {
                auto status = orchestrator_->RebuildContext(session_id);
                if (status.ok()) std::cout << "Context rebuilt." << std::endl;
                else std::cerr << "Error rebuilding context: " << status.message() << std::endl;
            }
        } else {
            std::cout << "Nothing to undo." << std::endl;
        }
        return Result::HANDLED;

    } else if (cmd == "/commit-vibe") {
        auto gid_or = db_->GetLastGroupId(session_id);
        if (!gid_or.ok() || gid_or->empty()) {
            std::cerr << "Error: No interaction history found to commit." << std::endl;
            return Result::HANDLED;
        }
        std::string gid = *gid_or;

        auto msgs_or = db_->GetMessagesByGroups({gid});
        std::string prompt;
        if (msgs_or.ok()) {
            for (const auto& m : *msgs_or) {
                if (m.role == "user") {
                    prompt = m.content;
                    break;
                }
            }
        }

        auto state_or = db_->GetSessionState(session_id);
        std::string state = state_or.ok() ? *state_or : "";

        std::string commit_msg = absl::Substitute(
            "vibe_id: $0\n\nprompt: $1\n\nstate:\n$2",
            gid, prompt, state);

        FILE* pipe = popen("git commit -F -", "w");
        if (!pipe) {
            std::cerr << "Error: Failed to execute git commit." << std::endl;
            return Result::HANDLED;
        }
        fwrite(commit_msg.c_str(), 1, commit_msg.size(), pipe);
        int status = pclose(pipe);
        if (status != 0) {
            std::cerr << "git commit failed. Ensure you have staged changes." << std::endl;
        } else {
            std::cout << "Vibe committed successfully with ID: " << gid << std::endl;
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
        } else {
            std::cout << "Usage: /tool [list|show <name>]" << std::endl;
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
        } else if (sub_cmd == "view" || sub_cmd == "show") {
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
                        std::string out = "ID: " + std::to_string(s.id) + "\nName: " + s.name + "\nDescription: " + s.description + "\nPatch:\n" + s.system_prompt_patch;
                        SmartDisplay(out);
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
            std::cout << "Unknown skill subcommand: " << sub_cmd << "\nUsage: /skill [activate|add|edit|show|delete|list]" << std::endl;
        }
        return Result::HANDLED;
    
    } else if (cmd == "/session") {
        if (args.empty()) {
            auto res = db_->Query("SELECT DISTINCT session_id FROM messages");
            if (res.ok()) log_status(PrintJsonAsTable(*res));
        } else {
            std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
            if (sub_parts[0] == "remove" && sub_parts.size() > 1) {
                std::string target = sub_parts[1];
                auto status = db_->DeleteSession(target);
                if (status.ok()) {
                    std::cout << "Session '" << target << "' removed successfully." << std::endl;
                    if (session_id == target) {
                        session_id = "default_session";
                        std::cout << "Switched to: " << session_id << std::endl;
                        if (orchestrator_) (void)orchestrator_->RebuildContext(session_id);
                    }
                } else {
                    std::cerr << "Error removing session: " << status.message() << std::endl;
                }
            } else {
                session_id = args;
                std::cout << "Switched to: " << session_id << std::endl;
                log_status(DisplayHistory(*db_, session_id, 20, selected_groups));
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
                if (quota_or.ok() && quota_or->is_object()) {
                    std::cout << "\n--- Gemini User Quota ---" << std::endl;
                    nlohmann::json table = nlohmann::json::array();
                    if (quota_or->contains("buckets") && (*quota_or)["buckets"].is_array()) {
                        for (const auto& b : (*quota_or)["buckets"]) { if (!b.is_object()) continue;
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
