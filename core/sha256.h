#ifndef SLOP_SHA256_H_
#define SLOP_SHA256_H_

#include <array>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace slop {

absl::StatusOr<std::array<unsigned char, 32>> Sha256Digest(absl::string_view input);

}  // namespace slop

#endif  // SLOP_SHA256_H_
