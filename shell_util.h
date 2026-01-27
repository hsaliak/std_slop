#ifndef SLOP_SHELL_UTIL_H_
#define SLOP_SHELL_UTIL_H_

#include <string>
#include "absl/status/statusor.h"

namespace slop {

// Executes a shell command and returns its stdout.
absl::StatusOr<std::string> RunCommand(const std::string& command);

}  // namespace slop

#endif  // SLOP_SHELL_UTIL_H_
