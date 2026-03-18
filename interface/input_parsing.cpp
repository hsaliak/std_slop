
#include "interface/input_parsing.h"

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace slop {

ParsedCommand ParseCommandInput(std::string_view input) {
  ParsedCommand parsed;
  absl::string_view trimmed = absl::StripLeadingAsciiWhitespace(input);
  if (trimmed.empty() || trimmed[0] != '/') {
    return parsed;
  }

  parsed.is_command = true;
  size_t pos = trimmed.find(' ');
  if (pos == absl::string_view::npos) {
    parsed.command = std::string(trimmed);
    return parsed;
  }

  parsed.command = std::string(trimmed.substr(0, pos));
  parsed.args = std::string(absl::StripAsciiWhitespace(trimmed.substr(pos + 1)));
  return parsed;
}

std::string RenderCommandInput(std::string_view command, std::string_view args) {
  if (args.empty()) {
    return std::string(command);
  }
  return absl::StrCat(command, " ", args);
}

}  // namespace slop