#ifndef SLOP_SHELL_UTIL_H_
#define SLOP_SHELL_UTIL_H_

#include <memory>
#include <string>

#include "absl/status/statusor.h"

#include "core/cancellation.h"

namespace slop {

struct CommandResult {
  std::string stdout_out;
  std::string stderr_out;
  int exit_code;
};

// Runs a shell command and returns the output and exit code.
// If cancellation is requested, the process and its children are killed.
absl::StatusOr<CommandResult> RunCommand(const std::string& command,
                                         std::shared_ptr<CancellationRequest> cancellation = nullptr);

// Escapes a string for use as a shell argument.
std::string EscapeShellArg(const std::string& arg);

// Checks if the Escape key was pressed.
// This function is non-blocking and throttled to once every 100ms.
// NOTE: Not thread-safe if called from multiple threads simultaneously.
bool IsEscPressed();

}  // namespace slop

#endif  // SLOP_SHELL_UTIL_H_
