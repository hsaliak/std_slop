#include "interface/ui.h"

// Role constants for message handling
namespace role_constants {
inline constexpr std::string_view kSystem = "system";
inline constexpr std::string_view kUser = "user";
inline constexpr std::string_view kAssistant = "assistant";
inline constexpr std::string_view kTool = "tool";
}  // namespace role_constants

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "absl/base/const_init.h"
#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "core/message_parser.h"
#include "interface/color.h"
#include "interface/command_definitions.h"
#include "interface/completer.h"
#include "markdown/parser.h"
#include "markdown/renderer.h"
#include "readline/history.h"
#include "readline/readline.h"

#include <sys/ioctl.h>
namespace slop {
namespace {
ABSL_CONST_INIT absl::Mutex g_ui_mu(absl::kConstInit);
}  // namespace
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
  std::string banner = R"(
███████╗████████╗██████╗       ███████╗██╗      ██████╗ ██████╗
██╔════╝╚══██╔══╝██╔══██╗██╗██╗██╔════╝██║     ██╔═══██╗██╔══██╗
███████╗   ██║   ██║  ██║╚═╝╚═╝███████╗██║     ██║   ██║██████╔╝
╚════██║   ██║   ██║  ██║██╗██╗╚════██║██║     ██║   ██║██╔═══╝
███████║   ██║   ██████╔╝╚═╝╚═╝███████║███████╗╚██████╔╝██║
╚══════╝   ╚═╝   ╚═════╝       ╚══════╝╚══════╝ ╚═════╝ ╚═╝
)";
  std::cout << Colorize(banner, "", ansi::Logo) << std::endl;
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
  char* buf = readline("❯ ");
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
std::string OpenInEditor(const std::string& initial_content, const std::string& extension) {
  const char* editor = std::getenv("EDITOR");
  if (!editor || std::string(editor).empty()) editor = "vi";
  std::string filename = absl::StrCat("slop_edit_", getpid(), "_", std::time(nullptr), extension);
  std::string tmp_path = (std::filesystem::temp_directory_path() / filename).string();
  {
    std::ofstream out(tmp_path);
    if (!initial_content.empty()) out << initial_content;
  }
  std::string cmd = absl::StrCat(editor, " ", tmp_path);
  int res = std::system(cmd.c_str());
  std::string content;
  std::ifstream in(tmp_path);
  if (in) {
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  if (std::filesystem::exists(tmp_path)) {
    std::filesystem::remove(tmp_path);
  }
  if (res != 0) {
    LOG(INFO) << "Editor exited with non-zero status: " << res;
    std::cerr << "Editor exited with non-zero status: " << res << std::endl;
    return "";
  }
  return content;
}
void SmartDisplay(const std::string& content, bool is_markdown) {
  const char* editor = std::getenv("EDITOR");
  if (editor && !std::string(editor).empty() && content.length() > 5000) {
    OpenInEditor(content, is_markdown ? ".md" : ".txt");
    return;
  }
  if (is_markdown) {
    PrintMarkdown(content);
  } else {
    std::cout << WrapText(content, GetTerminalWidth()) << std::endl;
  }
}

std::string FormatAssembledContext(const std::string& json_str) {
  auto j_top_opt = json_parse(json_str);
  if (!j_top_opt) return json_str;
  auto& j_top = *j_top_opt;
  if (j_top.is_discarded()) {
    return "Error parsing context JSON: " + json_str;
  }

  const nlohmann::json* j_ptr = &j_top;
  if (j_top.contains("request")) {
    j_ptr = &j_top["request"];
  }
  const nlohmann::json& j = *j_ptr;

  std::stringstream ss;
  ss << "# Assembled Context\n\n";

  // Handle Gemini format (system_instruction + contents)
  if (j.contains("system_instruction") || j.contains("contents")) {
    if (const auto *sys_instr = json_at(j, "system_instruction")) {
      ss << "## " << icons::Robot << " System Instruction\n\n";
      if (auto parts = json_get<nlohmann::json::array_t>(*sys_instr, "parts")) {
        for (const auto& part : *parts) {
          if (auto text = json_get<std::string>(part, "text")) {
            ss << *text << "\n\n";
          }
        }
      }
    }

    if (auto contents = json_get<nlohmann::json::array_t>(j, "contents")) {
      for (const auto& item : *contents) {
        std::string role = json_get_or(item, "role", std::string("unknown"));
        ss << "## " << (role == "user" ? icons::Input : icons::Robot) << " Role: " << role << "\n\n";
        if (auto parts = json_get<nlohmann::json::array_t>(item, "parts")) {
          for (const auto& part : *parts) {
            if (auto text = json_get<std::string>(part, "text")) {
              ss << *text << "\n\n";
            }
            if (const auto *fc = json_at(part, "functionCall")) {
              std::string name = json_get_or(*fc, "name", std::string("unknown"));
              ss << "### " << icons::Tool << " Tool Call: " << name << "\n\n";
              if (const auto *args = json_at(*fc, "args")) {
                if (name == "run_lua") {
                  ss << "```lua\n" << json_get_or(*args, "script", std::string{}) << "\n```\n\n";
                } else {
                  ss << "```json\n" << args->dump(2) << "\n```\n\n";
                }
              }
            }
            if (const auto *fr = json_at(part, "functionResponse")) {
              std::string name = json_get_or(*fr, "name", std::string("unknown"));
              ss << "### " << icons::Tool << " Tool Result: " << name << "\n\n";
              if (const auto *response = json_at(*fr, "response")) {
                ss << "```\n" << response->dump(2) << "\n```\n\n";
              }
            }
          }
        }
      }
    }
  }
  // Handle OpenAI format (messages)
  else if (auto messages = json_get<nlohmann::json::array_t>(j, "messages")) {
    for (const auto& msg : *messages) {
      std::string role = json_get_or(msg, "role", std::string("unknown"));
      ss << "## " << (role == "user" ? icons::Input : (role == "system" ? icons::Info : icons::Robot))
         << " Role: " << role << "\n\n";

      if (auto content = json_get<std::string>(msg, "content")) {
        ss << *content << "\n\n";
      }

      if (auto tool_calls = json_get<nlohmann::json::array_t>(msg, "tool_calls")) {
        for (const auto& call : *tool_calls) {
          if (const auto *fn = json_at(call, "function")) {
            std::string name = json_get_or(*fn, "name", std::string("unknown"));
            ss << "### " << icons::Tool << " Tool Call: " << name << "\n\n";

            std::string args_str = json_get_or(*fn, "arguments", std::string("{}"));
            auto args_opt = json_parse(args_str);
            if (args_opt && name == "run_lua") {
              ss << "```lua\n" << json_get_or(*args_opt, "script", std::string{}) << "\n```\n\n";
            } else if (args_opt) {
              ss << "```json\n" << args_opt->dump(2) << "\n```\n\n";
            } else {
              ss << "```\n" << args_str << "\n```\n\n";
            }
          }
        }
      }

      if (role == "tool") {
        std::string tool_id = json_get_or(msg, "tool_call_id", std::string("unknown"));
        ss << "### " << icons::Tool << " Tool Result (ID: " << tool_id << ")\n\n";
        ss << "```\n" << json_get_or(msg, "content", std::string{}) << "\n```\n\n";
      }
    }
  } else {
    // Fallback
    ss << "```json\n" << j.dump(2) << "\n```\n";
  }

  return ss.str();
}

void DisplayAssembledContext(const std::string& json_str) { SmartDisplay(FormatAssembledContext(json_str), true); }

void RenderMarkdown(const std::string& markdown, const std::string& prefix, std::string* rendered) {
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
  return renderer.Render(**parsed_or, rendered);
}
void PrintMarkdown(const std::string& markdown, const std::string& prefix) {
  std::string rendered;
  RenderMarkdown(markdown, prefix, &rendered);
  std::cout << rendered;
}
void PrintAssistantMessage(const std::string& content, const std::string& prefix, int tokens) {
  if (content.empty()) return;
  absl::MutexLock lock(&g_ui_mu);
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
  auto j_opt = json_parse(json_str);
  if (!j_opt) return json_str;
  auto& j = *j_opt;
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
void PrintToolCallMessage(const std::string& name, const std::string& args, const std::string& prefix, int tokens) {
  absl::MutexLock lock(&g_ui_mu);
  if (name == "run_lua") {
    auto j_opt = json_parse(args);
    if (!j_opt) return;
    auto& j = *j_opt;
    if (!j.is_discarded() && j.is_object() && j.contains("script")) {
      std::string script = j["script"];
      // Escape any existing backticks to avoid breaking our markdown code fence
      std::string escaped_script;
      escaped_script.reserve(script.size() + 64);
      size_t backtick_run = 0;
      for (char c : script) {
        if (c == '`') {
          backtick_run++;
          if (backtick_run == 3) {
            // We've hit 3 backticks, escape by inserting zero-width space
            escaped_script += "\xE2\x80\x8B";  // Zero-width space
            backtick_run = 1;
          }
          // Don't add the backtick yet
        } else {
          // Add any pending backticks
          for (size_t i = 0; i < backtick_run; i++) escaped_script += '`';
          backtick_run = 0;
          escaped_script += c;
        }
      }
      // Handle trailing backticks
      for (size_t i = 0; i < backtick_run; i++) escaped_script += '`';
      // Wrap in markdown code fence with lua syntax highlighting
      std::string markdown = absl::StrCat("```lua\n", escaped_script, "\n```");
      std::string summary = absl::StrCat(icons::Tool, " ", name, " (control plane)");
      std::cout << prefix << "    " << Colorize(summary, "", ansi::Metadata);
      if (tokens > 0) {
        std::cout << "  " << Colorize(absl::StrCat("· ", tokens, " tokens"), "", ansi::Metadata);
      }
      std::cout << std::endl;
      // Render the markdown (which includes syntax-highlighted Lua code)
      std::string rendered;
      RenderMarkdown(markdown, prefix, &rendered);
      // Split rendered output and print with prefix
      std::vector<std::string> rendered_lines = absl::StrSplit(rendered, '\n');
      for (const auto& line : rendered_lines) {
        std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " " << line << std::endl;
      }
      return;
    }
  }
  std::string display_args = FlattenJsonArgs(args);
  if (display_args.length() > 60) {
    display_args = display_args.substr(0, 57) + "...";
  }
  std::string summary = absl::StrCat(icons::Tool, " ", name, " ", icons::CallArrow, " ", display_args);
  std::cout << prefix << "    " << Colorize(summary, "", ansi::Metadata);
  if (tokens > 0) {
    std::cout << "  " << Colorize(absl::StrCat("· ", tokens, " tokens"), "", ansi::Metadata);
  }
  std::cout << std::endl;
}
void PrintToolResultMessage([[maybe_unused]] const std::string& name, const std::string& result,
                            const std::string& status, const std::string& prefix) {
  absl::MutexLock lock(&g_ui_mu);
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
  std::cout << prefix << "    " << Colorize("  │", "", ansi::Metadata) << " " << Colorize(summary, "", color)
            << std::endl;
  if (is_error && IsNetworkError(result)) {
    int printed = 0;
    for (const auto& line : out_lines) {
      if (printed >= 10) break;
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " " << std::string(line) << std::endl;
      printed++;
    }
    for (const auto& line : err_lines) {
      if (printed >= 10) break;
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " "
                << Colorize(std::string(line), "", ansi::Red) << std::endl;
      printed++;
    }
    if (out_lines.size() + err_lines.size() > static_cast<size_t>(printed)) {
      std::cout << prefix << "      " << Colorize("│", "", ansi::Metadata) << " ..." << std::endl;
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
  if (msg.role == std::string(role_constants::kUser)) {
    std::string label = absl::StrCat("User (GID: ", msg.group_id, ")> ");
    std::cout << "\n" << prefix << icons::Input << " " << Colorize(label, "", ansi::UserLabel) << std::endl;
    PrintStyledBlock(absl::StrCat(" > ", msg.content, " "), prefix, ansi::EchoFg, ansi::EchoBg);
  } else if (msg.role == std::string(role_constants::kAssistant)) {
    if (msg.status == "tool_call") {
      MessageContext ctx(msg);
      std::string text = MessageParser::ExtractAssistantText(ctx);
      if (!text.empty()) {
        PrintAssistantMessage(text, prefix + "  ", msg.tokens);
      }
      auto calls_or = MessageParser::ExtractToolCalls(ctx);
      if (calls_or.ok() && !calls_or->empty()) {
        for (const auto& call : *calls_or) {
          PrintToolCallMessage(call.name, call.args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                               prefix + "  ", msg.tokens);
        }
      } else if (!calls_or.ok() || calls_or->empty()) {
        // Fallback for unidentified tool calls
        PrintToolCallMessage("tool_call", msg.content, prefix + "  ", msg.tokens);
      }
    } else {
      PrintAssistantMessage(msg.content, prefix + "  ", msg.tokens);
    }
  } else if (msg.role == std::string(role_constants::kTool)) {
    PrintToolResultMessage(ExtractToolName(msg.tool_call_id), msg.content, msg.status, prefix + "  ");
  } else if (msg.role == std::string(role_constants::kSystem)) {
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
std::string GetHelpText() {
  std::string help =
      "# std::slop - The SQL-backed LLM CLI\n\n"
      "## Usage\n"
      "```bash\n"
      "std_slop [options] [session_id]\n"
      "```\n\n"
      "## Options\n"
      "- `--prompt \"...\"`: Run a single prompt in batch mode and exit.\n"
      "- `--session <id>`: Use a specific session ID (overrides positional argument).\n"
      "- `--model <name>`: Specify the model to use (e.g., `gpt-4o`, `claude-3-5-sonnet`).\n"
      "- `--helpfull`: See all available command-line flags.\n\n"
      "## Hotwords\n"
      "- `hey <skill> <query>`: Temporarily activate a skill for a single prompt. For example: `hey code_reviewer what "
      "do you think of this?`.\n\n"
      "## Slash Commands\n\n";
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> category_rows;
  std::vector<std::string> categories;
  for (const auto& def : slop::GetCommandDefinitions()) {
    if (std::find(categories.begin(), categories.end(), def.category) == categories.end()) {
      categories.push_back(def.category);
    }
    for (const auto& line : def.help_lines) {
      if (line.empty()) continue;
      if (line[0] == '/') {
        size_t sep = line.find("  ");
        if (sep != std::string::npos) {
          category_rows[def.category].emplace_back(line.substr(0, sep),
                                                   std::string(absl::StripLeadingAsciiWhitespace(line.substr(sep))));
        } else {
          category_rows[def.category].emplace_back(line, "");
        }
      } else {
        std::string name_part = def.name;
        for (const auto& alias : def.aliases) {
          name_part += ", " + alias;
        }
        category_rows[def.category].emplace_back(name_part, line);
      }
    }
  }
  for (const auto& cat : categories) {
    help += "### " + cat + "\n\n";
    help += "| Command | Description |\n";
    help += "| :--- | :--- |\n";
    for (const auto& row : category_rows[cat]) {
      // Escape pipes in markdown
      std::string cmd = absl::StrReplaceAll(row.first, {{"|", "\\|"}});
      std::string desc = absl::StrReplaceAll(row.second, {{"|", "\\|"}});
      help += absl::Substitute("| `$0` | $1 |\n", cmd, desc);
    }
    help += "\n";
  }
  return help;
}
void ShowHelp() { slop::PrintMarkdown(GetHelpText()); }
AsyncAnimator::AsyncAnimator() : is_running_(false) {
  frames_ = {
      {"   (  .      ) ", "   )           ( ", "  (  (   .  )  ) "},
      {"     )  .   (  ", "   (   (  .  )   ", "    )   )   (    "},
      {"   (   .    )  ", "    )   (   (    ", "  (   )   .  )   "},
  };
}

AsyncAnimator::~AsyncAnimator() { Stop(); }

void AsyncAnimator::Start() {
  if (is_running_) return;
  is_running_ = true;
  std::cout << "\033[?25l";
  std::cout << "\n\n\n\033[3A";
  thread_ = std::thread(&AsyncAnimator::RenderLoop, this);
}

void AsyncAnimator::Stop() {
  if (!is_running_) return;
  is_running_ = false;
  if (thread_.joinable()) thread_.join();
  
  for (int i = 0; i < 3; ++i) {
    std::cout << "\r\033[2K\n";
  }
  std::cout << "\033[3A";
  std::cout << "\033[?25h" << std::flush;
}

void AsyncAnimator::RenderLoop() {
  int frame_idx = 0;
  while (is_running_) {
    const auto& frame = frames_[frame_idx % frames_.size()];
    for (const auto& line : frame) {
      std::cout << "\r\033[2K\033[38;5;208m" << line << "\033[0m\n";
    }
    std::cout << "\033[3A" << std::flush;
    frame_idx++;
    for (int i = 0; i < 15 && is_running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

}  // namespace slop
