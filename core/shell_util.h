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

}  // namespace slop

#endif  // SLOP_SHELL_UTIL_H_
