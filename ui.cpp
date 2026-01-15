#include "ui.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <iomanip>
#include "absl/status/status.h"

namespace sentinel {

std::string OpenInEditor(const std::string& initial_content) {
    const char* editor = std::getenv("EDITOR");
    if (!editor) editor = "vi";
    
    std::string tmp_path = "/tmp/sentinel_edit.txt";
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

absl::Status DisplayHistory(sentinel::Database& db, const std::string& session_id, int limit) {
    auto history_or = db.GetConversationHistory(session_id);
    if (!history_or.ok()) return history_or.status();

    size_t start = history_or->size() > static_cast<size_t>(limit) ? history_or->size() - limit : 0;
    for (size_t i = start; i < history_or->size(); ++i) {
        const auto& msg = (*history_or)[i];
        if (msg.role == "user") {
            std::cout << "\n[User]> " << msg.content << std::endl;
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
    std::cout << std::endl;
    return absl::OkStatus();
}

} // namespace sentinel