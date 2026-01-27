#include "ui.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "nlohmann/json.hpp"

#include "color.h"
#include "completer.h"
#include "readline/history.h"
#include "readline/readline.h"

#include <sys/ioctl.h>
namespace slop {

namespace {

/**
 * @brief Calculates the printable length of a string, excluding ANSI escape codes.
 *
 * Handles multi-byte UTF-8 characters and standard ANSI SGR (Select Graphic Rendition)
 * sequences to determine how many columns the string will occupy in the terminal.
 *
 * @param s The string to measure.
 * @return size_t The number of visible terminal columns.
 */
size_t VisibleLength(const std::string& s) {
  size_t len = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    // Detect start of ANSI escape sequence
    if (s[i] == '\033' && i + 1 < s.length() && s[i + 1] == '[') {
      i += 2;
      // Skip characters until the termination character of the sequence (0x40-0x7E)
      while (i < s.length() && (s[i] < 0x40 || s[i] > 0x7E)) {
        i++;
      }
    } else {
      // For UTF-8, only count the start byte of a character sequence (bytes not 10xxxxxx)
      if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
        len++;
      }
    }
  }
  return len;
}

/**
 * @brief Prints a horizontal separator line to the terminal.
 *
 * @param width The width of the line. If 0, uses the current terminal width.
 * @param color_fg The ANSI color code for the line.
 * @param header Optional text to display centered within the line.
 */
void PrintHorizontalLine(size_t width, const char* color_fg = ansi::Grey, const std::string& header = "",
                         const std::string& prefix = "") {
  if (width == 0) width = GetTerminalWidth();
  size_t prefix_len = VisibleLength(prefix);
  std::string bold_fg = std::string(ansi::Bold) + color_fg;

  std::cout << prefix;
  if (header.empty()) {
    size_t line_width = (width > prefix_len) ? width - prefix_len : 0;
    std::string line(line_width, '-');
    std::cout << Colorize(line, "", bold_fg.c_str()) << std::endl;
  } else {
    std::string line = "[ " + header + " ]";
    std::cout << Colorize(line, "", bold_fg.c_str()) << std::endl;
  }
}

/**
 * @brief Renders text within a stylized section with a header.
 *
 * Automatically wraps the body text to fit within the terminal boundaries
 * and draws a horizontal separator using the specified color.
 *
 * @param header The title displayed at the top of the block.
 * @param body The main content of the block.
 * @param color_fg The ANSI color code for the header.
 * @param prefix Optional prefix for threading.
 */
void PrintStyledBlock(const std::string& body, const std::string& prefix,
                      const char* fg_color = ansi::White,
                      const char* bg_color = "") {
  size_t width = GetTerminalWidth();
  std::string wrapped = WrapText(body, width, prefix);
  std::vector<std::string> lines = absl::StrSplit(wrapped, '\n');

  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty() && i + 1 == lines.size()) continue;
    std::cout << Colorize(lines[i], bg_color, fg_color);
    if (i + 1 < lines.size()) std::cout << "\n";
  }
  std::cout << std::endl;
}
}  // namespace

size_t GetTerminalWidth() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col > 0 ? w.ws_col : 80;
  }
  return 80;
}

namespace {
std::vector<std::string> g_completion_commands;
absl::flat_hash_map<std::string, std::vector<std::string>> g_sub_commands;
std::vector<std::string> g_active_completion_list;

char* CommandGenerator(const char* text, int state) {
  static size_t list_index;
  static std::vector<std::string> matches;

  if (!state) {
    list_index = 0;
    matches = FilterCommands(text, g_active_completion_list);
  }

  if (list_index < matches.size()) {
    return strdup(matches[list_index++].c_str());
  }

  return nullptr;
}

char** CommandCompletionProvider(const char* text, int start, [[maybe_unused]] int end) {
  if (start == 0 && text[0] == '/') {
    g_active_completion_list = g_completion_commands;
    return rl_completion_matches(text, CommandGenerator);
  }
  if (start > 0) {
    std::string line(rl_line_buffer);
    std::vector<std::string> parts = absl::StrSplit(line, absl::MaxSplits(' ', 1));
    if (!parts.empty()) {
      auto it = g_sub_commands.find(parts[0]);
      if (it != g_sub_commands.end()) {
        g_active_completion_list = it->second;
        return rl_completion_matches(text, CommandGenerator);
      }
    }
  }
  return nullptr;
}

std::string ExtractToolName(const std::string& tool_call_id) {
  size_t pipe = tool_call_id.find('|');
  if (pipe != std::string::npos) {
    return tool_call_id.substr(pipe + 1);
  }
  return tool_call_id;
}
}  // namespace

void SetupTerminal() {}

void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands) {
  g_completion_commands = commands;
  g_sub_commands = sub_commands;
  rl_attempted_completion_function = CommandCompletionProvider;
  // Ensure '/' is not a word break character so we can complete /commands
  rl_basic_word_break_characters = const_cast<char*>(" \t\n\"\\'`@$><=;|&{(");
}

void ShowBanner() {
  std::cout << Colorize(R"(  ____ _____ ____               ____  _     ___  ____  )", "", ansi::Cyan) << std::endl;
  std::cout << Colorize(R"( / ___|_   _|  _ \     _   _   / ___|| |   / _ \|  _ \ )", "", ansi::Cyan) << std::endl;
  std::cout << Colorize(R"( \___ \ | | | | | |   (_) (_)  \___ \| |  | | | | |_) |)", "", ansi::Cyan) << std::endl;
  std::cout << Colorize(R"(  ___) || | | |_| |    _   _   |___) | |__| |_| |  __/ )", "", ansi::Cyan) << std::endl;
  std::cout << Colorize(R"( |____/ |_| |____/    (_) (_)  |____/|_____\___/|_|    )", "", ansi::Cyan) << std::endl;
  std::cout << std::endl;
#ifdef SLOP_VERSION
  std::cout << " std::slop version " << SLOP_VERSION << std::endl;
#endif
  std::cout << " Welcome to std::slop - The SQL-backed LLM CLI" << std::endl;
  std::cout << " Type /help for a list of commands." << std::endl;
  std::cout << std::endl;
}

std::string ReadLine(const std::string& modeline) {
  PrintHorizontalLine(0, ansi::Grey, modeline);
  char* buf = readline("> ");
  if (!buf) return "/exit";
  std::string line(buf);
  free(buf);
  if (!line.empty()) {
    add_history(line.c_str());
  }
  return line;
}

std::string WrapText(const std::string& text, size_t width, const std::string& prefix,
                     const std::string& first_line_prefix) {
  if (width == 0) width = GetTerminalWidth();
  size_t prefix_len = VisibleLength(prefix);
  size_t first_prefix_len = first_line_prefix.empty() ? prefix_len : VisibleLength(first_line_prefix);
  
  std::string result;
  std::string current_line;
  size_t current_line_visible_len = 0;
  bool is_first_line = true;

  auto finalize_line = [&]() {
    if (!result.empty()) result += "\n";
    if (is_first_line) {
      result += (first_line_prefix.empty() ? prefix : first_line_prefix) + current_line;
      is_first_line = false;
    } else {
      result += prefix + current_line;
    }
    current_line.clear();
    current_line_visible_len = 0;
  };

  size_t effective_width = (width > std::max(prefix_len, first_prefix_len) + 5) 
                           ? width - std::max(prefix_len, first_prefix_len) 
                           : width;

  std::stringstream ss(text);
  std::string paragraph;
  while (std::getline(ss, paragraph)) {
    std::stringstream word_ss(paragraph);
    std::string word;
    bool first_word = true;
    while (word_ss >> word) {
      size_t word_len = VisibleLength(word);
      if (!first_word && current_line_visible_len + 1 + word_len > effective_width) {
        finalize_line();
        first_word = true;
      }
      if (!first_word) {
        current_line += " ";
        current_line_visible_len += 1;
      }
      current_line += word;
      current_line_visible_len += word_len;
      first_word = false;
    }
    finalize_line();
  }

  return result;
}

std::string OpenInEditor(const std::string& initial_content) {
  const char* editor = std::getenv("EDITOR");
  if (!editor) editor = "vi";

  std::string tmp_path = (std::filesystem::temp_directory_path() / "slop_edit.txt").string();
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
    std::cout << WrapText(content, GetTerminalWidth()) << std::endl;
    return;
  }
  OpenInEditor(content);
}

std::string FormatAssembledContext(const std::string& json_str) {
  auto j_top = nlohmann::json::parse(json_str, nullptr, false);
  if (j_top.is_discarded()) {
    return "Error parsing context JSON: " + json_str;
  }

  const nlohmann::json* j_ptr = &j_top;
  if (j_top.contains("request")) {
    j_ptr = &j_top["request"];
  }
  const nlohmann::json& j = *j_ptr;

  std::stringstream ss;
  ss << "=== Assembled Context ===\n\n";

  if (j.contains("system_instruction")) {
    ss << "SYSTEM INSTRUCTION:\n";
    if (j["system_instruction"].contains("parts")) {
      for (const auto& part : j["system_instruction"]["parts"]) {
        if (part.contains("text")) ss << part["text"].get<std::string>() << "\n";
      }
    }
    ss << "\n";
  }

  if (j.contains("contents") && j["contents"].is_array()) {
    for (const auto& entry : j["contents"]) {
      std::string role = entry.value("role", "unknown");
      ss << "Role: " << role << "\n";
      if (entry.contains("parts")) {
        for (const auto& part : entry["parts"]) {
          if (part.contains("text")) ss << part["text"].get<std::string>() << "\n";
          if (part.contains("functionCall")) ss << "Function Call: " << part["functionCall"].dump() << "\n";
          if (part.contains("functionResponse")) ss << "Function Response: " << part["functionResponse"].dump() << "\n";
        }
      }
      ss << "\n";
    }
  } else if (j.contains("messages") && j["messages"].is_array()) {
    for (const auto& msg : j["messages"]) {
      std::string role = msg.value("role", "unknown");
      ss << "Role: " << role << "\n";
      if (msg.contains("content") && !msg["content"].is_null()) {
        ss << msg["content"].get<std::string>() << "\n";
      }
      if (msg.contains("tool_calls")) {
        ss << "Tool Calls: " << msg["tool_calls"].dump() << "\n";
      }
      if (msg.contains("tool_call_id")) {
        ss << "Tool Call ID: " << msg["tool_call_id"].get<std::string>() << "\n";
      }
      ss << "\n";
    }
  }
  return ss.str();
}

void DisplayAssembledContext(const std::string& json_str) { SmartDisplay(FormatAssembledContext(json_str)); }

absl::Status PrintJsonAsTable(const std::string& json_str) {
  auto j = nlohmann::json::parse(json_str, nullptr, false);
  if (j.is_discarded()) {
    return absl::InvalidArgumentError("Invalid JSON: " + json_str);
  }
  if (!j.is_array() || j.empty()) {
    std::cout << "No results found." << std::endl;
    return absl::OkStatus();
  }

  std::vector<std::string> keys;
  for (const auto& [key, value] : j[0].items()) keys.push_back(key);

  std::vector<size_t> widths(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) widths[i] = keys[i].length();

  for (const auto& row : j) {
    for (size_t i = 0; i < keys.size(); ++i) {
      std::string val;
      if (row.contains(keys[i])) {
        if (row[keys[i]].is_null())
          val = "NULL";
        else if (row[keys[i]].is_string())
          val = row[keys[i]].get<std::string>();
        else
          val = row[keys[i]].dump();
      }
      widths[i] = std::max(widths[i], val.length());
    }
  }

  auto print_sep = [&]() {
    std::cout << "+";
    for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
    std::cout << std::endl;
  };

  print_sep();
  std::cout << "|";
  for (size_t i = 0; i < keys.size(); ++i) {
    std::cout << " " << std::left << std::setw(widths[i]) << keys[i] << " |";
  }
  std::cout << std::endl;
  print_sep();

  for (const auto& row : j) {
    std::cout << "|";
    for (size_t i = 0; i < keys.size(); ++i) {
      std::string val;
      if (row.contains(keys[i])) {
        if (row[keys[i]].is_null())
          val = "NULL";
        else if (row[keys[i]].is_string())
          val = row[keys[i]].get<std::string>();
        else
          val = row[keys[i]].dump();
      } else {
        val = "";
      }
      if (val.length() > widths[i]) val = val.substr(0, widths[i] - 3) + "...";
      std::cout << " " << std::left << std::setw(widths[i]) << val << " |";
    }
    std::cout << std::endl;
  }
  print_sep();

  return absl::OkStatus();
}

void PrintThoughtMessage(const std::string& content, const std::string& prefix) {
  if (content.empty()) return;
  std::string thought = content;
  // Clean up markers if present
  thought = absl::StrReplaceAll(thought, {{"---THOUGHT---", ""}, {"---THOUGHT", ""}, {"---THOUGHTS---", ""}});
  thought = absl::StripAsciiWhitespace(thought);
  if (thought.empty()) return;

  PrintStyledBlock(thought, prefix + "    ", ansi::White);
  std::cout << std::endl;
}

void PrintAssistantMessage(const std::string& content, [[maybe_unused]] const std::string& skill_info,
                           const std::string& prefix, int tokens) {
  std::string remaining = content;
  size_t start = content.find("---THOUGHT---");
  if (start != std::string::npos) {
    size_t end = content.find("---", start + 13);
    if (end == std::string::npos) {
      end = content.find("\n\n", start + 13);
    }

    if (end != std::string::npos) {
      std::string thought = content.substr(start, end - start);
      PrintThoughtMessage(thought, prefix);
      remaining = content.substr(end);
      remaining = absl::StripLeadingAsciiWhitespace(remaining);
      if (absl::StartsWith(remaining, "---")) {
        size_t next_nl = remaining.find('\n');
        if (next_nl != std::string::npos) {
          remaining = remaining.substr(next_nl);
        }
      }
    }
  }

  remaining = absl::StripAsciiWhitespace(remaining);
  if (!remaining.empty()) {
    PrintStyledBlock(remaining, prefix + "    ", ansi::Assistant);
    if (tokens > 0) {
      std::cout << prefix << "    " << ansi::Grey << "· " << tokens << " tokens" << ansi::Reset << std::endl;
    }
  }
}

void PrintToolCallMessage(const std::string& name, const std::string& args, const std::string& prefix) {
  std::string display_args = args;
  // If args is JSON, try to make it more readable or compact
  auto j = nlohmann::json::parse(args, nullptr, false);
  if (!j.is_discarded()) {
      display_args = j.dump();
  }

  if (display_args.length() > 60) {
    display_args = display_args.substr(0, 57) + "...";
  }
  
  std::string summary = absl::StrCat(name, "(", display_args, ")");
  std::cout << prefix << "    " << Colorize(summary, "", ansi::Grey) << std::endl;
}

void PrintToolResultMessage(const std::string& name, const std::string& result, const std::string& status,
                            const std::string& prefix) {
  std::vector<absl::string_view> lines = absl::StrSplit(result, '\n');
  std::string summary = absl::Substitute("$0 ($1) - $2 lines", name, status, lines.size());
  const char* color = (status == "error" || absl::StartsWith(result, "Error:")) ? ansi::Red : ansi::Grey;

  // Indent more than the call for visual hierarchy
  std::cout << prefix << "        " << Colorize(summary, "", color) << std::endl;
}

absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit) {
  auto history_or = db.GetConversationHistory(session_id);
  if (!history_or.ok()) return history_or.status();

  size_t start = history_or->size() > static_cast<size_t>(limit) ? history_or->size() - limit : 0;
  for (size_t i = start; i < history_or->size(); ++i) {
    const auto& msg = (*history_or)[i];
    
    if (msg.role == "user") {
      std::cout << "\n" << Colorize("User (GID: " + msg.group_id + ")> ", "", ansi::Green) << std::endl;
      std::cout << WrapText(msg.content, GetTerminalWidth()) << std::endl;
    } else if (msg.role == "assistant") {
      if (msg.status == "tool_call") {
        auto j = nlohmann::json::parse(msg.content, nullptr, false);
        if (!j.is_discarded()) {
          if (j.contains("content") && j["content"].is_string()) {
            std::string content = j["content"];
            if (!content.empty()) {
              PrintAssistantMessage(content, "", "  ", msg.tokens);
            }
          }
          if (j.contains("functionCalls") && j["functionCalls"].is_array()) {
            for (const auto& call : j["functionCalls"]) {
              std::string name = call.contains("name") ? call["name"].get<std::string>() : "unknown";
              std::string args = call.contains("args") ? call["args"].dump() : "{}";
              PrintToolCallMessage(name, args, "  ");
            }
          } else {
            PrintToolCallMessage("tool_call", msg.content, "  ");
          }
        } else {
          PrintToolCallMessage("tool_call", msg.content, "  ");
        }
      } else {
        PrintAssistantMessage(msg.content, "", "  ", msg.tokens);
      }
    } else if (msg.role == "tool") {
      PrintToolResultMessage(ExtractToolName(msg.tool_call_id), msg.content, msg.status, "  ");
    } else if (msg.role == "system") {
      std::cout << Colorize("System> ", "", ansi::Yellow) << std::endl;
      std::cout << WrapText(msg.content, GetTerminalWidth()) << std::endl;
    }
  }
  return absl::OkStatus();
}

void HandleStatus(const absl::Status& status, const std::string& context) {
  if (status.ok()) return;
  if (!context.empty()) {
    LOG(ERROR) << context << ": " << status.message();
  } else {
    LOG(ERROR) << status.message();
  }
}

}  // namespace slop
