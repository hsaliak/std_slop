
#ifndef SLOP_INTERFACE_INPUT_PARSING_H_
#define SLOP_INTERFACE_INPUT_PARSING_H_

#include <string>
#include <string_view>

namespace slop {

struct ParsedCommand {
  bool is_command = false;
  std::string command;
  std::string args;
};

ParsedCommand ParseCommandInput(std::string_view input);
std::string RenderCommandInput(std::string_view command, std::string_view args);

}  // namespace slop

#endif  // SLOP_INTERFACE_INPUT_PARSING_H_