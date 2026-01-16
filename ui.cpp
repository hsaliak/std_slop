#include "ui.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <readline/readline.h>
#include <readline/history.h>
#include "absl/status/status.h"

namespace slop {

void SetupTerminal() {
    // Readline initialization
}

std::string ReadLine(const std::string& prompt, const std::string& session_id) {
    char* buf = readline(prompt.c_str());
    if (!buf) return "/exit";
    std::string line(buf);
    free(buf);
    if (!line.empty()) {
        add_history(line.c_str());
    }
    return line;
}

std::string OpenInEditor(const std::string& initial_content) {
    const char* editor = std::getenv("EDITOR");
    if (!editor) editor = "vi";
    
    std::string tmp_path = "/tmp/slop_edit.txt";
    { 
        std::ofstream out(tmp_path);
        if (!initial_content.empty()) out << initial_content;
    }

    std::string cmd = std::string(editor) + " " + tmp_path;
    int res = std::system(cmd.c_str());
    if (res != 0) return "";

    std::ifstream in(tmp_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::filesystem::remove(tmp_path);
    return content;
}

absl::Status PrintJsonAsTable(const std::string& json_str) {
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_discarded()) {
        return absl::InternalError("Failed to parse query results as JSON.");
    }
    
    if (!j.is_array() || j.empty()) {
        if (j.is_array() && j.empty()) {
            std::cout << "No results found." << std::endl;
            return absl::OkStatus();
        }
        return absl::InvalidArgumentError("JSON input is not a non-empty array.");
    }

    // Get keys from first object
    std::vector<std::string> keys;
    for (auto& [key, value] : j[0].items()) {
        keys.push_back(key);
    }

    if (keys.empty()) {
        std::cout << "No data columns found." << std::endl;
        return absl::OkStatus();
    }

    // Calculate column widths
    std::vector<size_t> widths(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        widths[i] = keys[i].length();
        for (const auto& row : j) {
            std::string val;
            if (row.contains(keys[i])) {
                if (row[keys[i]].is_null()) val = "NULL";
                else if (row[keys[i]].is_string()) val = row[keys[i]].get<std::string>();
                else val = row[keys[i]].dump();
            } else {
                val = "";
            }
            widths[i] = std::max(widths[i], val.length());
        }
    }

    // Print Header
    std::cout << "+";
    for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
    std::cout << "\n|";
    for (size_t i = 0; i < keys.size(); ++i) {
        std::cout << " " << std::left << std::setw(widths[i]) << keys[i] << " |";
    }
    std::cout << "\n+";
    for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
    std::cout << "\n";

    // Print Rows
    for (const auto& row : j) {
        std::cout << "|";
        for (size_t i = 0; i < keys.size(); ++i) {
            std::string val;
            if (row.contains(keys[i])) {
                if (row[keys[i]].is_null()) val = "NULL";
                else if (row[keys[i]].is_string()) val = row[keys[i]].get<std::string>();
                else val = row[keys[i]].dump();
            } else {
                val = "";
            }
            std::cout << " " << std::left << std::setw(widths[i]) << val << " |";
        }
        std::cout << "\n";
    }

    // Print Footer
    std::cout << "+";
    for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
    std::cout << std::endl;

    return absl::OkStatus();
}

absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit, const std::vector<std::string>& selected_groups) {
    auto history_or = db.GetConversationHistory(session_id);
    if (!history_or.ok()) return history_or.status();

    size_t start = history_or->size() > static_cast<size_t>(limit) ? history_or->size() - limit : 0;
    for (size_t i = start; i < history_or->size(); ++i) {
        const auto& msg = (*history_or)[i];
        if (msg.role == "user") {
            std::cout << "\n[User (GID: " << msg.group_id << ")]> " << msg.content << std::endl;
        } else if (msg.role == "assistant") {
            if (msg.status == "tool_call") {
                 std::cout << "[Assistant (Tool Call)]> " << msg.content << std::endl;
            } else {
                 std::cout << "[Assistant]> " << msg.content << std::endl;
            }
        } else if (msg.role == "tool") {
            std::cout << "[Tool Result]> " << msg.content << std::endl;
        } else if (msg.role == "system") {
            std::cout << "[System]> " << msg.content << std::endl;
        }
    }

    if (!selected_groups.empty()) {
        std::cout << "\n--- Injected Context (" << selected_groups.size() << " groups) ---" << std::endl;
        for (const auto& gid : selected_groups) {
            auto res = db.Query("SELECT role, substr(content, 1, 60) as preview FROM messages WHERE group_id = '" + gid + "' LIMIT 1");
            if (res.ok()) {
                auto j = nlohmann::json::parse(*res, nullptr, false);
                if (!j.is_discarded() && !j.empty()) {
                    std::cout << "  [" << gid << "] " << j[0]["role"].get<std::string>() << ": " << j[0]["preview"].get<std::string>() << "..." << std::endl;
                }
            }
        }
        std::cout << "------------------------------------------" << std::endl;
    }

    std::cout << std::endl;
    return absl::OkStatus();
}



void DisplayAssembledContext(const std::string& json_str) {
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_discarded()) {
        std::cout << "Error parsing assembled context." << std::endl;
        return;
    }

    // Unwrapping for GCA mode if necessary
    if (j.contains("request") && j["request"].is_object()) {
        j = j["request"];
    }

    std::cout << "\n\033[1;36m=== ASSEMBLED CONTEXT SENT TO LLM ===\033[0m\n" << std::endl;

    if (j.contains("system_instruction")) {
        std::cout << "\033[1;33m[SYSTEM INSTRUCTION]\033[0m" << std::endl;
        if (j["system_instruction"].contains("parts")) {
            for (const auto& part : j["system_instruction"]["parts"]) {
                if (part.contains("text")) std::cout << part["text"].get<std::string>() << std::endl;
            }
        }
        std::cout << "\033[1;30m------------------------------------------\033[0m" << std::endl;
    }

    if (j.contains("contents")) {
        // Gemini
        for (const auto& item : j["contents"]) {
            std::string role = item["role"];
            if (role == "user") std::cout << "\033[1;32m[USER]\033[0m" << std::endl;
            else if (role == "model") std::cout << "\033[1;34m[ASSISTANT]\033[0m" << std::endl;
            else if (role == "function") std::cout << "\033[1;35m[FUNCTION RESPONSE]\033[0m" << std::endl;
            else std::cout << "\033[1;35m[" << role << "]\033[0m" << std::endl;

            for (const auto& part : item["parts"]) {
                if (part.contains("text")) {
                    std::cout << part["text"].get<std::string>() << std::endl;
                } else if (part.contains("functionCall")) {
                    std::cout << "\033[1;35mCALL:\033[0m " << part["functionCall"].dump(2) << std::endl;
                } else if (part.contains("functionResponse")) {
                    std::cout << "\033[1;35mRESPONSE:\033[0m " << part["functionResponse"].dump(2) << std::endl;
                }
            }
            std::cout << "\033[1;30m------------------------------------------\033[0m" << std::endl;
        }
    } else if (j.contains("messages")) {
        // OpenAI
        for (const auto& msg : j["messages"]) {
            std::string role = msg["role"];
            if (role == "user") std::cout << "\033[1;32m[USER]\033[0m" << std::endl;
            else if (role == "assistant") std::cout << "\033[1;34m[ASSISTANT]\033[0m" << std::endl;
            else if (role == "system") std::cout << "\033[1;33m[SYSTEM]\033[0m" << std::endl;
            else if (role == "tool") std::cout << "\033[1;35m[TOOL RESPONSE]\033[0m" << std::endl;
            else std::cout << "\033[1;35m[" << role << "]\033[0m" << std::endl;

            if (msg.contains("content") && msg["content"].is_string()) {
                std::cout << msg["content"].get<std::string>() << std::endl;
            }
            if (msg.contains("tool_calls")) {
                std::cout << "\033[1;35mTOOL CALLS:\033[0m " << msg["tool_calls"].dump(2) << std::endl;
            }
            if (msg.contains("name")) {
                std::cout << "\033[1;30m(Name: " << msg["name"].get<std::string>() << ")\033[0m" << std::endl;
            }
            std::cout << "\033[1;30m------------------------------------------\033[0m" << std::endl;
        }
    }
    
    std::cout << "\033[1;36m=== END OF ASSEMBLED CONTEXT ===\033[0m\n" << std::endl;
}

} // namespace slop
