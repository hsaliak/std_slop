#include "core/shell_util.h"
#include "ini/ini_parser.h"

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"

namespace slop {

IniConfig ParseIni(std::string_view content) {
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

      // Handle inline comments
      size_t comment_pos = value_view.find_first_of("#;");
      if (comment_pos != std::string_view::npos) {
        value_view = value_view.substr(0, comment_pos);
      }

      std::string value = ExpandEnvVars(std::string(absl::StripAsciiWhitespace(value_view)));
      if (!key.empty()) {
        config[current_section][key] = value;
      }
    }
  }

  return config;
}

}  // namespace slop
