#include "interface/ui.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "absl/base/no_destructor.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "nlohmann/json.hpp"

#include "core/message_parser.h"
#include "interface/color.h"
#include "interface/completer.h"
#include "markdown/parser.h"
#include "markdown/renderer.h"
#include "readline/history.h"
#include "readline/readline.h"

#include <sys/ioctl.h>
namespace slop {

namespace {

bool IsNetworkError(const std::string& result) {
  std::string lower = absl::AsciiStrToLower(result);
  return absl::StrContains(result, "400") || absl::StrContains(result, "429") || absl::StrContains(result, "503") ||
         absl::StrContains(lower, "http error") || absl::StrContains(lower, "too many requests") ||
         absl::StrContains(lower, "rate limit") || absl::StrContains(lower, "rate_limit") ||
         absl::StrContains(lower, "resource exhausted") || absl::StrContains(lower, "resource_exhausted") ||
         absl::StrContains(lower, "quota");
}

/**
 * @brief Prints a horizontal separator line to the terminal.
 *
 * @param width The width of the line. If 0, uses the current terminal width.
 * @param color_fg The ANSI color code for the line.
 * @param header Optional text to display centered within the line.
 */
void PrintHorizontalLine(size_t width, const char* color_fg = ansi::Metadata, const std::string& header = "",
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
void PrintStyledBlock(const std::string& body, const std::string& prefix, const char* fg_color = ansi::White,
                      const char* bg_color = "") {
  size_t width = GetTerminalWidth();
  // We use first_line_prefix to apply the prefix to all lines but only if the prefix is not already in the body.
  // Actually WrapText already handles the prefix.
  std::string wrapped = WrapText(body, width, prefix);
  std::vector<std::string> lines = absl::StrSplit(wrapped, '\n');

  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].empty() && i + 1 == lines.size()) continue;

    // If fg_color is Assistant (White), we wrap each line in it, but we MUST
    // respect internal color codes. Colorize usually wraps everything.
    // If body already has ANSI codes, we should be careful.
    if (fg_color && *fg_color) {
      std::cout << fg_color << bg_color << lines[i] << ansi::Reset;
    } else {
      std::cout << lines[i];
    }

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

markdown::MarkdownParser& GetMarkdownParser() {
  static absl::NoDestructor<markdown::MarkdownParser> parser;
  return *parser;
}

markdown::MarkdownRenderer& GetMarkdownRenderer() {
  static absl::NoDestructor<markdown::MarkdownRenderer> renderer;
  return *renderer;
}
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

void SetupTerminal() {
  // Ensure the terminal is not in "Application Cursor Keys" mode or "Keypad" mode.
  // These modes often cause terminals to send arrow key sequences (like \033OA)
  // on mouse scroll, which readline interprets as history navigation instead of
  // allowing the terminal to scroll its buffer.
  // \033[?1l: Disable Application Cursor Keys (DECCKM)
  // \033>: Disable Keypad Mode (DECPNM)
  std::cout << "\033[?1l\033>" << std::flush;
}

void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands) {
  g_completion_commands = commands;
  g_sub_commands = sub_commands;
  rl_attempted_completion_function = CommandCompletionProvider;
  // Ensure '/' is not a word break character so we can complete /commands
  rl_basic_word_break_characters = const_cast<char*>(" \t\n\"\\'`@$><=;|&{(");
}

void ShowBanner() {
  std::cout << Colorize(R"(  ____ _____ ____               ____  _     ___  ____  )", "", ansi::Logo) << std::endl;
  std::cout << Colorize(R"( / ___|_   _|  _ \     _   _   / ___|| |   / _ \|  _ \ )", "", ansi::Logo) << std::endl;
  std::cout << Colorize(R"( \___ \ | | | | | |   (_) (_)  \___ \| |  | | | | |_) |)", "", ansi::Logo) << std::endl;
  std::cout << Colorize(R"(  ___) || | | |_| |    _   _   |___) | |__| |_| |  __/ )", "", ansi::Logo) << std::endl;
  std::cout << Colorize(R"( |____/ |_| |____/    (_) (_)  |____/|_____\___/|_|    )", "", ansi::Logo) << std::endl;
  std::cout << std::endl;
#ifdef SLOP_VERSION
  std::cout << " std::slop version " << SLOP_VERSION << std::endl;
#endif
  std::cout << " Welcome to std::slop - The SQL-backed LLM CLI" << std::endl;
  std::cout << " Type /help for a list of commands." << std::endl;
  std::cout << std::endl;
}

std::string ReadLine(const std::string& modeline) {
  SetupTerminal();
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

  size_t effective_width =
      (width > std::max(prefix_len, first_prefix_len) + 5) ? width - std::max(prefix_len, first_prefix_len) : width;

  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (VisibleLength(line) <= effective_width) {
      current_line = line;
      finalize_line();
      continue;
    }

    std::stringstream word_ss(line);
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
          if (part.contains("functionCall"))
            ss << "Function Call: "
               << part["functionCall"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
          if (part.contains("functionResponse"))
            ss << "Function Response: "
               << part["functionResponse"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
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
        ss << "Tool Calls: " << msg["tool_calls"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
           << "\n";
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

void PrintMarkdown(const std::string& markdown, const std::string& prefix) {
  auto& parser = GetMarkdownParser();
  auto& renderer = GetMarkdownRenderer();

  auto parsed_or = parser.Parse(markdown);
  if (!parsed_or.ok()) {
    std::cout << prefix << markdown << std::endl;
    return;
  }

  size_t width = GetTerminalWidth();
  size_t prefix_len = VisibleLength(prefix);
  renderer.SetMaxWidth(width > prefix_len + 5 ? width - prefix_len : 0);

  std::string rendered;
  renderer.Render(**parsed_or, &rendered);
  std::cout << WrapText(rendered, width, prefix) << std::endl;
}

void PrintAssistantMessage(const std::string& content, const std::string& prefix, int tokens) {
  if (content.empty()) return;

  auto parsed_or = GetMarkdownParser().Parse(content);
  if (parsed_or.ok()) {
    std::string rendered;
    GetMarkdownRenderer().Render(**parsed_or, &rendered);
    PrintStyledBlock(rendered, prefix + "    ", ansi::Assistant);
  } else {
    PrintStyledBlock(content, prefix + "    ", ansi::Assistant);
  }
  if (tokens > 0) {
    std::cout << prefix << "    " << ansi::Metadata << "· " << tokens << " tokens" << ansi::Reset << std::endl;
  }
}

std::string FlattenJsonArgs(const std::string& json_str) {
  auto j = nlohmann::json::parse(json_str, nullptr, false);
  if (j.is_discarded()) {
    return json_str;
  }
  if (!j.is_object()) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }
  std::vector<std::string> parts;
  for (const auto& [key, value] : j.items()) {
    parts.push_back(absl::StrCat(key, ": ", value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)));
  }
  return absl::StrJoin(parts, " | ");
}

void PrintToolCallMessage(const std::string& name, const std::string& args, const std::string& prefix) {
  std::string display_args = FlattenJsonArgs(args);

  if (display_args.length() > 60) {
    display_args = display_args.substr(0, 57) + "...";
  }

  std::string summary = absl::StrCat(icons::Tool, " ", name, " ", icons::CallArrow, " ", display_args);
  std::cout << prefix << "    " << Colorize(summary, "", ansi::Metadata) << std::endl;
}

void PrintToolResultMessage(const std::string& /*name*/, const std::string& result, const std::string& status,
                            const std::string& prefix) {
  // Split into stdout and stderr
  std::string stdout_part = result;
  std::string stderr_part;
  size_t stderr_pos = result.find("### STDERR\n");
  if (stderr_pos != std::string::npos) {
    stdout_part = result.substr(0, stderr_pos);
    stderr_part = result.substr(stderr_pos + 11);
  }

  std::vector<absl::string_view> out_lines =
      absl::StrSplit(absl::StripAsciiWhitespace(stdout_part), '\n', absl::SkipEmpty());
  std::vector<absl::string_view> err_lines =
      absl::StrSplit(absl::StripAsciiWhitespace(stderr_part), '\n', absl::SkipEmpty());

  bool is_error = (status == "error" || absl::StartsWith(result, "Error:"));
  const char* color = is_error ? ansi::Red : ansi::Metadata;

  // Print Summary
  std::string summary =
      absl::Substitute("$0 $1 ($2 lines)", is_error ? icons::Error : icons::Success, status, out_lines.size());
  std::cout << prefix << "    " << Colorize(icons::ResultConnector, "", ansi::Metadata) << " "
            << Colorize(summary, "", color) << std::endl;

  if (is_error && IsNetworkError(result)) {
    for (const auto& line : out_lines) {
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " " << std::string(line) << std::endl;
    }
    for (const auto& line : err_lines) {
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " "
                << Colorize(std::string(line), "", ansi::Red) << std::endl;
    }
  } else {
    // Print stderr summary if present
    if (!err_lines.empty()) {
      std::string err_summary = absl::Substitute("[stderr: $0 lines omitted]", err_lines.size());
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " "
                << Colorize(err_summary, "", ansi::Red) << std::endl;
    }
  }
}

void PrintMessage(const Database::Message& msg, const std::string& prefix) {
  if (msg.role == "user") {
    std::string label = absl::StrCat("User (GID: ", msg.group_id, ")> ");
    std::cout << "\n" << prefix << icons::Input << " " << Colorize(label, "", ansi::UserLabel) << std::endl;
    PrintStyledBlock(absl::StrCat(" > ", msg.content, " "), prefix, ansi::EchoFg, ansi::EchoBg);
  } else if (msg.role == "assistant") {
    if (msg.status == "tool_call") {
      std::string text = MessageParser::ExtractAssistantText(msg);
      if (!text.empty()) {
        PrintAssistantMessage(text, prefix + "  ", msg.tokens);
      }

      auto calls_or = MessageParser::ExtractToolCalls(msg);
      if (calls_or.ok() && !calls_or->empty()) {
        for (const auto& call : *calls_or) {
          PrintToolCallMessage(call.name, call.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                               prefix + "  ");
        }
      } else if (!calls_or.ok() || calls_or->empty()) {
        // Fallback for unidentified tool calls
        PrintToolCallMessage("tool_call", msg.content, prefix + "  ");
      }
    } else {
      PrintAssistantMessage(msg.content, prefix + "  ", msg.tokens);
    }
  } else if (msg.role == "tool") {
    PrintToolResultMessage(ExtractToolName(msg.tool_call_id), msg.content, msg.status, prefix + "  ");
  } else if (msg.role == "system") {
    std::cout << prefix << icons::Info << " " << Colorize("System> ", "", ansi::SystemLabel) << std::endl;
    std::cout << WrapText(msg.content, GetTerminalWidth(), prefix) << std::endl;
  }
}

absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit) {
  auto history_or = db.GetConversationHistory(session_id);
  if (!history_or.ok()) return history_or.status();

  size_t start = history_or->size() > static_cast<size_t>(limit) ? history_or->size() - limit : 0;
  for (size_t i = start; i < history_or->size(); ++i) {
    PrintMessage((*history_or)[i]);
  }
  return absl::OkStatus();
}

void HandleStatus(const absl::Status& status, const std::string& context) {
  if (status.ok()) return;

  std::string msg(status.message());
  std::string log_msg = msg;
  if (size_t first_nl = log_msg.find('\n'); first_nl != std::string::npos) {
    log_msg = log_msg.substr(0, first_nl) + " (multi-line)...";
  }
  if (log_msg.length() > 100) {
    log_msg = log_msg.substr(0, 97) + "...";
  }

  if (!context.empty()) {
    std::cerr << icons::Error << " " << context << ": " << log_msg << std::endl;
    LOG(WARNING) << context << ": " << log_msg;
  } else {
    std::cerr << icons::Error << " " << log_msg << std::endl;
    LOG(WARNING) << log_msg;
  }
}

}  // namespace slop
