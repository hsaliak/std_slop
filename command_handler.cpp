#include "command_handler.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/match.h"

namespace sentinel {

CommandHandler::Result CommandHandler::Handle(std::string& input, std::string& session_id, std::vector<std::string>& active_skills, std::function<void()> show_help_fn) {
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
        if (sub_cmd == "drop") {
            log_status(db_->Execute("UPDATE messages SET status = 'dropped' WHERE session_id = '" + session_id + "'"));
        } else if (sub_cmd == "build") {
            int n = sub_args.empty() ? 5 : std::atoi(sub_args.c_str());
            std::string sub = "SELECT DISTINCT group_id FROM messages WHERE session_id = '" + session_id + "' ORDER BY created_at DESC LIMIT " + std::to_string(n);
            log_status(db_->Execute("UPDATE messages SET status = 'completed' WHERE group_id IN (" + sub + ")"));
        } else if (sub_cmd == "show") {
            log_status(DisplayHistory(*db_, session_id, 20));
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
            log_status(db_->SetContextMode(session_id, Database::ContextMode::FULL, -1));
            std::cout << "Full Context Mode enabled." << std::endl;
        } else {
            auto s = db_->GetContextSettings(session_id);
            if (s.ok()) std::cout << "Current Mode: " << (s->mode == Database::ContextMode::FTS_RANKED ? "FTS_RANKED" : "FULL") << " (Size: " << s->size << ")" << std::endl;
            std::cout << "Usage: /context-mode fts <N> | full" << std::endl;
        }
        return Result::HANDLED;
    } else if (cmd == "/skills") {
        auto s_or = db_->GetSkills();
        if (s_or.ok()) {
            for (const auto& s : *s_or) {
                bool active = false;
                for (const auto& a : active_skills) if (a == s.name) active = true;
                std::cout << (active ? "* " : "  ") << "[" << s.id << "] " << s.name << std::endl;
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/skill") {
        std::vector<std::string> sub_parts = absl::StrSplit(args, absl::MaxSplits(' ', 1));
        if (sub_parts[0] == "activate") {
            auto s_or = db_->GetSkills();
            if (s_or.ok()) {
                for (const auto& s : *s_or) {
                    if (s.name == sub_parts[1] || (std::atoi(sub_parts[1].c_str()) > 0 && s.id == std::atoi(sub_parts[1].c_str()))) {
                        active_skills = {s.name};
                        std::cout << "Activated: " << s.name << std::endl;
                        return Result::HANDLED;
                    }
                }
            }
        }
        return Result::HANDLED;
    } else if (cmd == "/sessions") {
        auto res = db_->Query("SELECT DISTINCT session_id FROM messages");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
        return Result::HANDLED;
    } else if (cmd == "/switch") {
        if (!args.empty()) { session_id = args; std::cout << "Switched to: " << session_id << std::endl; }
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
        if (res.ok()) log_status(PrintJsonAsTable(*res));
        return Result::HANDLED;
    } else if (cmd == "/schema") {
        auto res = db_->Query("SELECT name, sql FROM sqlite_master WHERE type='table'");
        if (res.ok()) log_status(PrintJsonAsTable(*res));
        return Result::HANDLED;
    } else if (cmd == "/model") {
        return Result::PROCEED_TO_LLM;
    }
    std::cout << "Unknown command: " << cmd << std::endl;
    return Result::UNKNOWN;
}

}  // namespace sentinel
