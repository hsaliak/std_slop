#ifndef SLOP_SHELL_UTIL_H_
#define SLOP_SHELL_UTIL_H_

#include <memory>
#include <string>
#include <string_view>

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
absl::StatusOr<CommandResult> RunCommand(std::string_view command,
                                         std::shared_ptr<CancellationRequest> cancellation = nullptr,
                                         std::string_view input = "", int timeout_seconds = 0);

// Escapes a string for use as a shell argument.
std::string EscapeShellArg(std::string_view arg);

// ScopedRAII class to enter raw mode for efficient terminal polling.
class ScopedRawMode {
 public:
  ScopedRawMode();
  ~ScopedRawMode();

  // Returns true if raw mode was successfully entered.
  bool IsActive() const;
};

// Checks if the Escape key was pressed.
// This function is non-blocking and throttled to once every 100ms.
// If a ScopedRawMode is active, it uses the existing terminal state.
// NOTE: Not thread-safe if called from multiple threads simultaneously.
bool IsInterruptPressed();

// Returns the current user's home directory.
std::string GetHomeDir();

// Expands environment variables in the format ${VAR_NAME} or $VAR_NAME.
std::string ExpandEnvVars(const std::string& input);

}  // namespace slop

#endif  // SLOP_SHELL_UTIL_H_
