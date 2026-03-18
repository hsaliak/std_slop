
#include "tools/tool_dispatcher.h"

#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ValidateCallRejectsMissingIdentifiers(const std::string& args_json) {
  nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
  if (args.is_discarded()) args = nlohmann::json::object();
  ToolDispatcher::Call missing_id{"", "query_db", args};
  ToolDispatcher::Call missing_name{"call_1", "", args};

  const absl::Status id_status = ToolDispatcher::ValidateCall(missing_id);
  const absl::Status name_status = ToolDispatcher::ValidateCall(missing_name);
  EXPECT_FALSE(id_status.ok());
  EXPECT_FALSE(name_status.ok());
}

void ValidateCallDeterministicForArbitraryArgs(const std::string& id, const std::string& name,
                                               const std::string& args_json) {
  nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
  if (args.is_discarded()) args = nlohmann::json::object();
  ToolDispatcher::Call call{id, name, args};
  const absl::Status first = ToolDispatcher::ValidateCall(call);
  const absl::Status second = ToolDispatcher::ValidateCall(call);
  EXPECT_EQ(first.ok(), second.ok());
  EXPECT_EQ(first.code(), second.code());
}

void SubmitNeverExecutesInvalidCalls(const std::string& id, const std::string& name, const std::string& args_json) {
  nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
  if (args.is_discarded()) args = nlohmann::json::object();

  int execute_count = 0;
  ToolDispatcher dispatcher([&execute_count](const std::string&, const nlohmann::json&,
                                             std::shared_ptr<CancellationRequest>) -> absl::StatusOr<std::string> {
    ++execute_count;
    return std::string("ok");
  });

  ToolDispatcher::Call call{id, name, args};
  auto job = dispatcher.Submit(call);
  auto result = job->Wait();

  if (!id.empty() && !name.empty()) {
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(execute_count, 1);
  } else {
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(execute_count, 0);
  }
}

FUZZ_TEST(ToolDispatcherValidationFuzzTest, ValidateCallRejectsMissingIdentifiers)
    .WithDomains(fuzztest::Arbitrary<std::string>());

FUZZ_TEST(ToolDispatcherValidationFuzzTest, ValidateCallDeterministicForArbitraryArgs)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>());

FUZZ_TEST(ToolDispatcherValidationFuzzTest, SubmitNeverExecutesInvalidCalls)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop