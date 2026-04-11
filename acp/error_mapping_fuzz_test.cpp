
#include "acp/error_mapping.h"

#include "acp/rpc_envelope.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void MapStatusToAcpErrorNoCrash(int status_code_raw, const std::string& message) {
  const auto status_code = static_cast<absl::StatusCode>(status_code_raw % 32);
  const absl::Status status(status_code, message);

  const AcpError err = MapStatusToAcpError(status);
  EXPECT_TRUE(err.code == kInvalidRequestCode || err.code == kInternalErrorCode);

  const nlohmann::json response = MakeAcpErrorResponse(nlohmann::json("id"), err);
  EXPECT_EQ(response.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_TRUE(response.contains("error"));
}

FUZZ_TEST(ErrorMappingFuzzTest, MapStatusToAcpErrorNoCrash);

}  // namespace
}  // namespace slop::acp
