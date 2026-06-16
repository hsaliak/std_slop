
#include "tools/tool_dispatcher.h"
#include "tools/tool_executor.h"

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

#include "core/cancellation.h"
#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void DispatcherValidateCallRejectsInvalidShape(const std::string& id, const std::string& name, const std::string& args_raw) {
  nlohmann::json args = nlohmann::json::parse(args_raw, nullptr, false);
  ToolDispatcher::Call call{id, name, args};

  const absl::Status status = ToolDispatcher::ValidateCall(call);
  if (id.empty() || name.empty() || args.is_discarded()) {
    EXPECT_FALSE(status.ok());
    return;
  }
  EXPECT_TRUE(status.ok());
}

void DispatcherSubmitNoCrash(const std::string& id, const std::string& name, const std::string& args_raw) {
  nlohmann::json args = nlohmann::json::parse(args_raw, nullptr, false);
  ToolDispatcher dispatcher(
      [](const std::string& tool_name, const nlohmann::json& tool_args, std::shared_ptr<CancellationRequest>) {
        return absl::StatusOr<std::string>(tool_name + ":" + std::to_string(tool_args.size()));
      });

  const auto job = dispatcher.Submit({id, name, args}, std::make_shared<CancellationRequest>());
  ASSERT_TRUE(job != nullptr);
  const auto result = job->Wait();
  if (id.empty() || name.empty() || args.is_discarded()) {
    EXPECT_FALSE(result.ok());
    return;
  }
  EXPECT_TRUE(result.ok());
}

void ExecutorUnknownToolRejected(const std::string& name, const std::string& args_raw) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  nlohmann::json args = nlohmann::json::parse(args_raw, nullptr, false);
  if (args.is_discarded()) {
    args = nlohmann::json::object();
  }

  const auto result = executor.Execute(name, args);
  if (name.empty()) {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
    return;
  }

  if (name != "read_file" && name != "write_file" && name != "query_db" && name != "execute_bash" && name != "list_directory" &&
      name != "describe_db" && name != "grep" && name != "edit_tool" && name != "read_scratchpad" &&
      name != "write_scratchpad" && name != "use_skill" && name != "git_create_staging_branch" &&
      name != "git_commit_patch" && name != "git_format_patch_series" && name != "git_reroll_patch" &&
      name != "git_verify_series" && name != "git_finalize_series") {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
  }
}

FUZZ_TEST(DispatcherExecutorBoundaryFuzzTest, DispatcherValidateCallRejectsInvalidShape);

FUZZ_TEST(DispatcherExecutorBoundaryFuzzTest, DispatcherSubmitNoCrash);

FUZZ_TEST(DispatcherExecutorBoundaryFuzzTest, ExecutorUnknownToolRejected)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_./-]{0,48}"), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop