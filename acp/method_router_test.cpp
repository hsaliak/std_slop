
#include "acp/method_router.h"

#include <gtest/gtest.h>

#include "core/database.h"

namespace slop::acp {
namespace {

absl::StatusOr<std::string> PromptExecStub(const std::string&, const std::string&,
                                           std::shared_ptr<slop::CancellationRequest>,
                                           const SessionUpdateWriter&) {
  return std::string("ok");
}

TEST(MethodRouterBoundaryValidationTest, RejectsSessionNewBeforeInitializeWithoutSideEffects) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  MethodRouter router;
  NegotiatedRuntimeOptions state;

  RpcRequest request;
  request.id = 1;
  request.method = "session/new";
  request.params = nlohmann::json::object();

  DispatchOutcome out = router.Dispatch(request, &state, &db, PromptExecStub);
  ASSERT_TRUE(out.has_response);
  EXPECT_EQ(out.response.at("error").at("message").get<std::string>(), "initialize_required");

  auto stmt_or = db.Prepare("SELECT COUNT(*) FROM sessions");
  ASSERT_TRUE(stmt_or.ok());
  ASSERT_TRUE((*stmt_or)->Step().ok());
  EXPECT_EQ((*stmt_or)->ColumnInt(0), 0);
}

TEST(MethodRouterBoundaryValidationTest, RejectsPromptInvalidShapeWithoutRegisteringInFlight) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  MethodRouter router;
  NegotiatedRuntimeOptions state;
  state.initialized = true;

  RpcRequest request;
  request.id = 2;
  request.method = "session/prompt";
  request.params = nlohmann::json::array({"bad"});

  DispatchOutcome out = router.Dispatch(request, &state, &db, PromptExecStub);
  ASSERT_TRUE(out.has_response);
  EXPECT_EQ(out.response.at("error").at("message").get<std::string>(), "session_prompt_params_must_be_object");

  auto cancellation = router.FindInFlightPrompt("acp_1");
  EXPECT_EQ(cancellation, nullptr);
}

TEST(MethodRouterBoundaryValidationTest, RejectsUnknownMethodAtBoundary) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  MethodRouter router;
  NegotiatedRuntimeOptions state;
  state.initialized = true;

  RpcRequest request;
  request.id = "abc";
  request.method = "session/unknown";
  request.params = nlohmann::json::object();

  DispatchOutcome out = router.Dispatch(request, &state, &db, PromptExecStub);
  ASSERT_TRUE(out.has_response);
  EXPECT_EQ(out.response.at("error").at("code").get<int>(), kMethodNotFoundCode);
}

}  // namespace
}  // namespace slop::acp
