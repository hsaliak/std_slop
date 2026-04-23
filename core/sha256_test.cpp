#include "core/sha256.h"

#include "gtest/gtest.h"

namespace slop {
namespace {

TEST(Sha256Test, ProducesKnownDigestForPkceVector) {
  auto digest_or = Sha256Digest("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  ASSERT_TRUE(digest_or.ok());
  const std::array<unsigned char, 32>& digest = *digest_or;
  EXPECT_EQ(digest[0], 0x13);
  EXPECT_EQ(digest[1], 0xd3);
  EXPECT_EQ(digest[2], 0x1e);
  EXPECT_EQ(digest[3], 0x96);
  EXPECT_EQ(digest[31], 0xc3);
}

}  // namespace
}  // namespace slop
