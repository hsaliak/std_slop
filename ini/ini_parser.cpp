#include "ini/ini_parser.h"

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"

#include "core/shell_util.h"

namespace slop {

IniConfig ParseIni(std::string_view content, bool expand_env_vars) {
  IniConfig config;
  std::string current_section;

  for (std::string_view line : absl::StrSplit(content, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }

    if (line[0] == '[' && line.back() == ']') {
      current_section = std::string(absl::StripAsciiWhitespace(line.substr(1, line.size() - 2)));
      continue;
    }

    size_t eq_pos = line.find('=');
    if (eq_pos != std::string_view::npos) {
      std::string key = std::string(absl::StripAsciiWhitespace(line.substr(0, eq_pos)));
      std::string_view value_view = line.substr(eq_pos + 1);

      bool in_quotes = false;
      bool escaped = false;
      for (size_t i = 0; i < value_view.size(); ++i) {
        const char c = value_view[i];
        if (in_quotes) {
          if (escaped) {
            escaped = false;
          } else if (c == '\\') {
            escaped = true;
          } else if (c == '"') {
            in_quotes = false;
          }
          continue;
        }
        if (c == '"') {
          in_quotes = true;
          continue;
        }
        if ((c == '#' || c == ';') && (i == 0 || absl::ascii_isspace(value_view[i - 1]))) {
          value_view = value_view.substr(0, i);
          break;
        }
      }

      std::string value(absl::StripAsciiWhitespace(value_view));
      if (expand_env_vars) value = ExpandEnvVars(value);
      if (!key.empty()) {
        config[current_section][key] = value;
      }
    }
  }

  return config;
}

}  // namespace slop
