
#include "acp/error_mapping.h"

#include "acp/rpc_envelope.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(ErrorMappingTest, ParseAndMethodErrorsUseJsonRpcCodes) {
  const AcpError parse = MakeParseError();
  EXPECT_EQ(parse.code, kParseErrorCode);
  EXPECT_EQ(parse.message, "Parse error");

  const AcpError missing = MakeMethodNotFoundError("nope");
  EXPECT_EQ(missing.code, kMethodNotFoundCode);
  EXPECT_NE(missing.message.find("nope"), std::string::npos);
}

TEST(ErrorMappingTest, StatusMappingUsesStableCodes) {
  const AcpError invalid = MapStatusToAcpError(absl::InvalidArgumentError("bad"));
  EXPECT_EQ(invalid.code, kInvalidRequestCode);
  EXPECT_EQ(invalid.message, "bad");

  const AcpError internal = MapStatusToAcpError(absl::InternalError("boom"));
  EXPECT_EQ(internal.code, kInternalErrorCode);
  EXPECT_EQ(internal.message, "boom");

  const AcpError unknown = MapStatusToAcpError(absl::UnknownError("opaque"));
  EXPECT_EQ(unknown.code, kInvalidRequestCode);
  EXPECT_EQ(unknown.message, "opaque");
}

TEST(ErrorMappingTest, ErrorResponseShapeStable) {
  nlohmann::json id = 7;
  const nlohmann::json response = MakeAcpErrorResponse(id, MakeInvalidRequestError("bad_req"));

  EXPECT_EQ(response.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_EQ(response.at("id").get<int>(), 7);
  EXPECT_EQ(response.at("error").at("code").get<int>(), kInvalidRequestCode);
  EXPECT_EQ(response.at("error").at("message").get<std::string>(), "bad_req");
}

}  // namespace
}  // namespace slop::acp
