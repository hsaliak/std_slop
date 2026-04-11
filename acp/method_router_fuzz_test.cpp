
#include "acp/method_router.h"

#include <string>

#include "core/database.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void DispatchBoundaryValidationNoCrash(const std::string& method,
                                       int id_mode,
                                       int params_mode,
                                       const std::string& session_id,
                                       const std::string& prompt) {
  Database db;
  if (!db.Init(":memory:").ok()) {
    return;
  }

  MethodRouter router;
  NegotiatedRuntimeOptions state;
  state.initialized = true;

  int executor_calls = 0;
  PromptExecutor executor = [&executor_calls](const std::string&, const std::string&,
                                              std::shared_ptr<slop::CancellationRequest>)
      -> absl::StatusOr<std::string> {
    ++executor_calls;
    return std::string("ok");
  };

  RpcRequest request;
  if (id_mode % 2 == 0) {
    request.id = nlohmann::json(1);
  }
  request.method = method;

  switch (params_mode % 4) {
    case 0:
      request.params = nlohmann::json::object({{"sessionId", session_id}, {"prompt", prompt}});
      break;
    case 1:
      request.params = nlohmann::json::array({session_id, prompt});
      break;
    case 2:
      request.params = nlohmann::json("not_object");
      break;
    default:
      request.params = nlohmann::json::object();
      break;
  }

  DispatchOutcome out = router.Dispatch(request, &state, &db, executor);
  if (!request.id.has_value()) {
    EXPECT_FALSE(out.has_response);
  }

  const bool prompt_like = request.method == "session/prompt";
  const bool params_object = request.params.is_object();
  if (prompt_like && !params_object) {
    EXPECT_EQ(executor_calls, 0);
  }
}

FUZZ_TEST(MethodRouterFuzzTest, DispatchBoundaryValidationNoCrash);

}  // namespace
}  // namespace slop::acp
