
#include "tools/common.h"

#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ValidateExecuteBashArgsDeterministic(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = nlohmann::json::object();
  }

  const absl::Status first = ValidateExecuteBashArgs(args);
  const absl::Status second = ValidateExecuteBashArgs(args);
  EXPECT_EQ(first.ok(), second.ok());
  EXPECT_EQ(first.code(), second.code());
}

void ExecuteBashRequiresCommand(const std::string& cmd, bool has_command) {
  nlohmann::json args = nlohmann::json::object();
  if (has_command) {
    args["command"] = cmd;
  }

  const absl::Status status = ValidateExecuteBashArgs(args);
  EXPECT_EQ(status.ok(), has_command && !cmd.empty());
}

void ExecuteBashTimeoutAndTypes(const std::string& command, int timeout_seconds, bool allow_nonzero_exit) {
  nlohmann::json args = nlohmann::json::object();
  args["command"] = command;
  args["timeout_seconds"] = timeout_seconds;
  args["allow_nonzero_exit"] = allow_nonzero_exit;

  const absl::Status status = ValidateExecuteBashArgs(args);
  if (command.empty() || timeout_seconds < 0) {
    EXPECT_FALSE(status.ok());
    return;
  }
  EXPECT_TRUE(status.ok());
}

FUZZ_TEST(ExecToolArgsFuzzTest, ValidateExecuteBashArgsDeterministic);

FUZZ_TEST(ExecToolArgsFuzzTest, ExecuteBashRequiresCommand)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(ExecToolArgsFuzzTest, ExecuteBashTimeoutAndTypes)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<int>(), fuzztest::Arbitrary<bool>());

}  // namespace
}  // namespace slop