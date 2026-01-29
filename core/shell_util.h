#ifndef SLOP_SHELL_UTIL_H_
#define SLOP_SHELL_UTIL_H_

#include <string>

#include "absl/status/statusor.h"

namespace slop {

struct CommandResult {
  std::string stdout_out;
  std::string stderr_out;
  int exit_code;
};

// Executes a shell command and returns its stdout and exit code.
absl::StatusOr<CommandResult> RunCommand(const std::string& command);

// Escapes a string for use as a single shell argument.
std::string EscapeShellArg(const std::string& arg);

}  // namespace slop

#endif  // SLOP_SHELL_UTIL_H_
