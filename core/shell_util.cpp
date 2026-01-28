#include "core/shell_util.h"

#include <array>
#include <cstdio>
#include <memory>

#include "absl/status/status.h"

namespace slop {

absl::StatusOr<std::string> RunCommand(const std::string& command) {
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
  if (status != 0) {
    return absl::InternalError("Command failed with status " + std::to_string(status) + ": " + result);
  }
  return result;
}

}  // namespace slop
