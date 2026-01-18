#include "ui.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <readline/readline.h>
#include <readline/history.h>
#include "absl/status/status.h"
#include "color.h"

namespace slop {

namespace {
size_t VisibleLength(const std::string& s) {
    size_t len = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\033' && i + 1 < s.length() && s[i+1] == '[') {
            i += 2;
            while (i < s.length() && (s[i] < 0x40 || s[i] > 0x7E)) {
                i++;
            }
        } else {
            len++;
        }
    }
    return len;
}
} // namespace

void SetupTerminal() {
    // Readline initialization
}

void ShowBanner() {
    std::cout << Colorize(R"(  ____ _____ ____               ____  _     ___  ____  )", ansi::BlueBg) << std::endl;
    std::cout << Colorize(R"( / ___|_   _|  _ \     _   _   / ___|| |   / _ \|  _ \ )", ansi::BlueBg) << std::endl;
    std::cout << Colorize(R"( \___ \ | | | | | |   (_) (_)  \___ \| |  | | | | |_) |)", ansi::BlueBg) << std::endl;
    std::cout << Colorize(R"(  ___) || | | |_| |    _   _   |___) | |__| |_| |  __/ )", ansi::BlueBg) << std::endl;
    std::cout << Colorize(R"( |____/ |_| |____/    (_) (_)  |____/|_____\___/|_|    )", ansi::BlueBg) << std::endl;
    std::cout << std::endl;
    std::cout << " Welcome to std::slop - The SQL-backed LLM CLI" << std::endl;
    std::cout << " Type /help for a list of commands." << std::endl;
    std::cout << std::endl;
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

std::string WrapText(const std::string& text, size_t width) {
    std::string result;
    std::string current_line;
    size_t current_line_visible_len = 0;

    auto finalize_line = [&]() {
        if (!result.empty()) result += "\n";
        result += current_line;
        current_line.clear();
        current_line_visible_len = 0;
    };

    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\n') {
            finalize_line();
            continue;
        }

        if (text[i] == ' ') {
            continue;
        }

        size_t j = i;
        while (j < text.length() && text[j] != ' ' && text[j] != '\n') {
            j++;
        }
        
        std::string word = text.substr(i, j - i);
        size_t word_len = VisibleLength(word);

        if (current_line_visible_len + (current_line_visible_len > 0 ? 1 : 0) + word_len > width) {
            if (current_line_visible_len > 0) {
                finalize_line();
            }
        }

        if (current_line_visible_len > 0) {
            current_line += " ";
            current_line_visible_len += 1;
        }
        current_line += word;
        current_line_visible_len += word_len;

        i = j - 1;
    }

    if (!current_line.empty() || (result.empty() && !text.empty())) {
        if (!result.empty() && !current_line.empty()) result += "\n";
        result += current_line;
    }
    return result;
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

void SmartDisplay(const std::string& content) {
    const char* editor = std::getenv("EDITOR");
    if (!editor || std::string(editor).empty()) {
        std::cout << WrapText(content) << std::endl;
        return;
    }

    std::string tmp_path = "/tmp/slop_view.txt";
    { 
        std::ofstream out(tmp_path);
        out << content;
    }

    std::string cmd = std::string(editor) + " " + tmp_path;
    int res = std::system(cmd.c_str());
    std::filesystem::remove(tmp_path);

    if (res != 0) {
        std::cout << WrapText(content) << std::endl;
    }
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

    std::vector<std::string> keys;
    for (auto& [key, value] : j[0].items()) {
        keys.push_back(key);
    }

    if (keys.empty()) {
        std::cout << "No data columns found." << std::endl;
        return absl::OkStatus();
    }

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
        widths[i] = std::min(widths[i], static_cast<size_t>(40));
    }

    auto print_sep = [&]() {
        std::cout << "+";
        for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
        std::cout << "\n";
    };

    print_sep();
    std::cout << "|";
    for (size_t i = 0; i < keys.size(); ++i) {
        std::string k = keys[i];
        if (k.length() > widths[i]) k = k.substr(0, widths[i]-1) + "~";
        std::cout << " " << std::left << std::setw(widths[i]) << k << " |";
    }
    std::cout << "\n";
    print_sep();

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
            if (val.length() > widths[i]) val = val.substr(0, widths[i]-1) + "~";
            std::cout << " " << std::left << std::setw(widths[i]) << val << " |";
        }
        std::cout << "\n";
    }
    print_sep();

    return absl::OkStatus();
}

absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit, const std::vector<std::string>& selected_groups) {
    auto history_or = db.GetConversationHistory(session_id);
    if (!history_or.ok()) return history_or.status();

    size_t start = history_or->size() > static_cast<size_t>(limit) ? history_or->size() - limit : 0;
    for (size_t i = start; i < history_or->size(); ++i) {
        const auto& msg = (*history_or)[i];
        if (msg.role == "user") {
            std::cout << "\n[User (GID: " << msg.group_id << ")]> " << WrapText(msg.content, 70) << std::endl;
        } else if (msg.role == "assistant") {
            if (msg.status == "tool_call") {
                 std::cout << Colorize("[Assistant (Tool Call)]> " + WrapText(msg.content, 70), ansi::CyanBg) << std::endl;
            } else {
                 std::cout << Colorize("[Assistant]> " + WrapText(msg.content, 70), ansi::BlueBg) << std::endl;
            }
        } else if (msg.role == "tool") {
            std::cout << Colorize("[Tool Result]> " + WrapText(msg.content, 70), ansi::GreyBg) << std::endl;
        } else if (msg.role == "system") {
            std::cout << "[System]> " << WrapText(msg.content, 70) << std::endl;
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

std::string FormatAssembledContext(const std::string& json_str) {
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_discarded()) {
        return "Error parsing assembled context.";
    }

    if (j.contains("request") && j["request"].is_object()) {
        j = j["request"];
    }

    std::stringstream ss;
    ss << "\n=== ASSEMBLED CONTEXT SENT TO LLM ===\n" << std::endl;

    if (j.contains("system_instruction")) {
        ss << "[SYSTEM INSTRUCTION]" << std::endl;
        if (j["system_instruction"].contains("parts")) {
            for (const auto& part : j["system_instruction"]["parts"]) {
                if (part.contains("text")) ss << WrapText(part["text"].get<std::string>(), 78) << std::endl;
            }
        }
        ss << "------------------------------------------" << std::endl;
    }

    if (j.contains("contents")) {
        for (const auto& content : j["contents"]) {
            std::string role = content.value("role", "unknown");
            ss << "[" << role << "]" << std::endl;
            if (content.contains("parts")) {
                for (const auto& part : content["parts"]) {
                    if (part.contains("text")) {
                        ss << WrapText(part["text"].get<std::string>(), 78) << std::endl;
                    } else if (part.contains("functionCall")) {
                        ss << "CALL: " << part["functionCall"]["name"].get<std::string>() 
                           << "(" << part["functionCall"]["args"].dump() << ")" << std::endl;
                    } else if (part.contains("functionResponse")) {
                        ss << "RESPONSE [" << part["functionResponse"]["name"].get<std::string>() << "]: " 
                           << part["functionResponse"]["response"].dump() << std::endl;
                    }
                }
            }
            ss << "------------------------------------------" << std::endl;
        }
    }

    return ss.str();
}

void DisplayAssembledContext(const std::string& json_str) {
    SmartDisplay(FormatAssembledContext(json_str));
}

} // namespace slop
