#ifndef SLOP_INTERFACE_COMMAND_DEFINITIONS_H_
#define SLOP_INTERFACE_COMMAND_DEFINITIONS_H_

#include <string>
#include <vector>

namespace slop {

struct CommandDefinition {
  std::string name;
  std::vector<std::string> sub_commands;
  std::vector<std::string> aliases;
  std::vector<std::string> help_lines;
  std::string category;
};

const std::vector<CommandDefinition>& GetCommandDefinitions();

}  // namespace slop

#endif  // SLOP_INTERFACE_COMMAND_DEFINITIONS_H_
