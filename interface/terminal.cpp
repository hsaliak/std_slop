#include "interface/terminal.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <sstream>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"

#include "interface/color.h"

#include <readline/history.h>
#include <readline/readline.h>
#include <sys/ioctl.h>

namespace slop {
namespace {

std::string g_readline_prefill;
std::string g_history_path;

std::string MakeRule(size_t count) {
  std::string out;
  out.reserve(count * 3);
  for (size_t i = 0; i < count; ++i) out += "─";
  return out;
}

std::string ResolveHistoryPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return ".slop_history";
  }
  return std::string(home) + "/.slop_history";
}

void FlushReadlineHistory() {
  if (g_history_path.empty()) return;
  write_history(g_history_path.c_str());
  history_truncate_file(g_history_path.c_str(), 2000);
}

int PrefillReadlineBuffer() {
  if (!g_readline_prefill.empty()) {
    rl_insert_text(g_readline_prefill.c_str());
    rl_point = rl_end;
    g_readline_prefill.clear();
  }
  rl_startup_hook = nullptr;
  return 0;
}

}  // namespace

size_t GetTerminalWidth() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col > 0 ? w.ws_col : 80;
  }
  return 80;
}

void SetupTerminal() {
  static bool readline_configured = false;
  static char kReadlineName[] = "slop";

  rl_catch_signals = 0;
  if (!readline_configured) {
    rl_readline_name = kReadlineName;
    // Force readline initialization first so config-file/default processing has
    // already happened before we apply our app-level preference.
    rl_initialize();
    // Middle-click paste may include embedded newlines; bracketed paste keeps
    // those as literal input instead of triggering accept-line.
    if (rl_variable_bind("enable-bracketed-paste", "on") != 0) {
      std::cerr << "warning: failed to enable readline bracketed paste" << std::endl;
    }
    if (rl_variable_bind("horizontal-scroll-mode", "off") != 0) {
      std::cerr << "warning: failed to disable readline horizontal scroll mode" << std::endl;
    }
    g_history_path = ResolveHistoryPath();
    using_history();
    stifle_history(2000);
    read_history(g_history_path.c_str());
    std::atexit(FlushReadlineHistory);
    readline_configured = true;
  }

  // Ensure the terminal doesn't echo weird codes
  // \033[?1l: Disable Application Cursor Keys (DECCKM)
  // \033>: Disable Keypad Mode (DECPNM)
  std::cout << "\033[?1l\033>" << std::flush;
}

void PrintHorizontalLine(size_t width, const char* color_fg, const std::string& header, const char* color_header) {
  if (width == 0) width = GetTerminalWidth() - 1;

  std::string line;
  if (header.empty()) {
    line = MakeRule(width);
  } else {
    size_t header_len = VisibleLength(header);
    if (header_len + 4 >= width) {
      line = header;
    } else {
      size_t side = (width - header_len - 2) / 2;
      line = MakeRule(side) + " " + color_header + header + color_fg + " " +
             MakeRule(width - side - header_len - 2);
    }
  }
  std::cout << color_fg << line << ansi::Reset << std::endl;
}

std::string WrapText(const std::string& text, size_t width, const std::string& prefix,
                     const std::string& first_line_prefix) {
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
  std::string line_in;
  while (std::getline(ss, line_in)) {
    if (VisibleLength(line_in) <= effective_width) {
      current_line = line_in;
      finalize_line();
      continue;
    }
    std::stringstream word_ss(line_in);
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

std::string ReadLine(const std::string& modeline, const std::string& initial_input) {
  SetupTerminal();
  if (!modeline.empty()) {
    PrintHorizontalLine(0, ansi::Grey, modeline, ansi::Grey);
  }

  bool hold_once_armed = false;
  std::string held_multiline_input = initial_input;

  while (true) {
    if (!held_multiline_input.empty()) {
      g_readline_prefill = held_multiline_input;
      rl_startup_hook = PrefillReadlineBuffer;
    }

    char* buf = readline("❯ ");
    if (!buf) return "/exit";

    std::string line(buf);
    free(buf);

    const bool has_newline = absl::StrContains(line, "\n") || absl::StrContains(line, "\r");
    if (!hold_once_armed && has_newline) {
      hold_once_armed = true;
      held_multiline_input = line;
      std::cout << ansi::Yellow << "[pasted block — press Enter to send]" << ansi::Reset << std::endl;
      continue;
    }

    const absl::string_view stripped = absl::StripAsciiWhitespace(line);
    if (!stripped.empty()) {
      HIST_ENTRY* last = history_get(history_length);
      const bool duplicate_of_last = (last != nullptr && last->line != nullptr && line == last->line);
      if (!duplicate_of_last) {
        add_history(line.c_str());
      }
    }
    return line;
  }
}

}  // namespace slop
