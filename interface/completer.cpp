#include "interface/completer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <readline/readline.h>
#include <readline/history.h>

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"

namespace slop {

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
}  // namespace

std::vector<std::string> FilterCommands(const std::string& prefix, const std::vector<std::string>& commands) {
  if (prefix.empty()) return commands;

  std::vector<std::string> filtered;
  for (const auto& cmd : commands) {
    if (absl::StartsWith(cmd, prefix)) {
      filtered.push_back(cmd);
    }
  }
  std::sort(filtered.begin(), filtered.end());
  return filtered;
}

void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands) {
  g_completion_commands = commands;
  g_sub_commands = sub_commands;
  rl_attempted_completion_function = CommandCompletionProvider;
  // Ensure '/' is not a word break character so we can complete /commands
  rl_basic_word_break_characters = const_cast<char*>(" \t\n\"\\'`@$><=;|&{(");
}

}  // namespace slop
