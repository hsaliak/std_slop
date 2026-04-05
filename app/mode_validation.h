
#ifndef SLOP_APP_MODE_VALIDATION_H_
#define SLOP_APP_MODE_VALIDATION_H_

#include <string>

#include "absl/status/status.h"

namespace slop {

absl::Status ValidateModeFlags(bool acp_mode, const std::string& prompt);

}  // namespace slop

#endif  // SLOP_APP_MODE_VALIDATION_H_