
#include "app/mode_validation.h"

#include "absl/status/status.h"

namespace slop {

absl::Status ValidateModeFlags(bool acp_mode, const std::string& prompt) {
  if (acp_mode && !prompt.empty()) {
    return absl::InvalidArgumentError("--acp and --prompt are mutually exclusive.");
  }
  return absl::OkStatus();
}

}  // namespace slop