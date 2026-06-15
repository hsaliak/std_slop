#include <string>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "tools/tool_executor.h"

namespace slop {
namespace {

std::string BuildBlockedToolName(bool use_llm_query, const std::string& suffix) {
  if (use_llm_query) {
    return "llm_query";
  }
  if (suffix.empty()) {
    return "llm_tool_x";
  }
  return "llm_tool_" + suffix;
}

void SubqueryPolicyBoundaryNoCrash(bool use_llm_query, const std::string& suffix, int depth) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string tool_name = BuildBlockedToolName(use_llm_query, suffix);
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, depth);

  const auto result = executor.Execute(tool_name, nlohmann::json::object());

  if (depth > 1) {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
    return;
  }

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

void SubqueryPolicyDeterministic(bool use_llm_query, const std::string& suffix, int depth) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string tool_name = BuildBlockedToolName(use_llm_query, suffix);
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, depth);

  const auto first = executor.Execute(tool_name, nlohmann::json::object());
  const auto second = executor.Execute(tool_name, nlohmann::json::object());

  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    EXPECT_EQ(first.status().message(), second.status().message());
  }
}

void JsBridgeSubqueryPolicyNoBypass(bool use_llm_query, const std::string& suffix, int depth) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  const std::string tool_name = BuildBlockedToolName(use_llm_query, suffix);
  executor.RegisterTool(tool_name, [](const nlohmann::json&, std::shared_ptr<CancellationRequest>) {
    return absl::StatusOr<std::string>("should not run");
  });
  ASSERT_TRUE(db.RegisterTool({tool_name, "blocked", "{}", true}).ok());
  executor.SetExecutionContext(ToolExecutor::ExecutionScope::kSubquery, depth);

  const std::string escaped_tool_name = json_dump(nlohmann::json(tool_name));
  const auto result = executor.Execute("run_js", {{"code", "return call_tool(" + escaped_tool_name + ", {});"}});

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  if (depth <= 1) {
    EXPECT_TRUE(absl::StrContains(result.status().message(), "not allowed in subquery scope"));
  }
}

FUZZ_TEST(SubqueryPolicyFuzzTest, SubqueryPolicyBoundaryNoCrash)
    .WithDomains(fuzztest::Arbitrary<bool>(), fuzztest::InRegexp("[A-Za-z0-9_.-]{0,32}"), fuzztest::InRange(-4, 8))
    .WithSeeds(std::vector<std::tuple<bool, std::string, int>>{
        std::make_tuple(true, std::string(""), 1),
        std::make_tuple(false, std::string("foo"), 1),
        std::make_tuple(false, std::string("reviewer"), 2),
    });

FUZZ_TEST(SubqueryPolicyFuzzTest, SubqueryPolicyDeterministic)
    .WithDomains(fuzztest::Arbitrary<bool>(), fuzztest::InRegexp("[A-Za-z0-9_.-]{0,32}"), fuzztest::InRange(-4, 8))
    .WithSeeds(std::vector<std::tuple<bool, std::string, int>>{
        std::make_tuple(true, std::string(""), 1),
        std::make_tuple(false, std::string("foo"), 1),
        std::make_tuple(false, std::string("reviewer"), 2),
    });

FUZZ_TEST(SubqueryPolicyFuzzTest, JsBridgeSubqueryPolicyNoBypass)
    .WithDomains(fuzztest::Arbitrary<bool>(), fuzztest::InRegexp("[A-Za-z0-9_.-]{0,32}"), fuzztest::InRange(0, 3))
    .WithSeeds(std::vector<std::tuple<bool, std::string, int>>{
        std::make_tuple(true, std::string(""), 1),
        std::make_tuple(false, std::string("researcher"), 1),
        std::make_tuple(false, std::string("reviewer"), 2),
    });

}  // namespace
}  // namespace slop
