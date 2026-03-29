#include "tools/tool_executor.h"

#include <string>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

std::string BuildBlockedToolName(bool use_llm_query, const std::string& suffix) {
  if (use_llm_query) {
    return "llm_query";
  }
  if (suffix.empty()) {
    return "llm_tool.x";
  }
  return "llm_tool." + suffix;
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

}  // namespace
}  // namespace slop
