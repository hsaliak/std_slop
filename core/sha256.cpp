#include "core/sha256.h"

#include "absl/status/status.h"
#include "mbedtls/sha256.h"

namespace slop {

absl::StatusOr<std::array<unsigned char, 32>> Sha256Digest(absl::string_view input) {
  std::array<unsigned char, 32> digest;
  const int result = mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(),
                                    /*is224=*/0);
  if (result != 0) {
    return absl::InternalError("mbedtls_sha256 failed");
  }
  return digest;
}

}  // namespace slop
