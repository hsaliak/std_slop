#include "core/shell_util.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <memory>

#include "absl/status/status.h"

namespace slop {

absl::StatusOr<CommandResult> RunCommand(const std::string& command) {
  std::array<char, 128> buffer;
  std::string result;
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return absl::InternalError("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }
  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return CommandResult{result, exit_code};
}

}  // namespace slop
