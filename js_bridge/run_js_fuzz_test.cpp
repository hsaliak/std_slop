
#include <string>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "fuzztest/fuzztest.h"
#include "js_bridge/interpreter.h"

namespace slop {
namespace {

void ValidateRunJsArgsDeterministic(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = raw_args;
  }

  const absl::Status first = ValidateRunJsArgs(args);
  const absl::Status second = ValidateRunJsArgs(args);
  EXPECT_EQ(first.ok(), second.ok());
  EXPECT_EQ(first.code(), second.code());
}

void RunJsArgsRequireCodeString(const std::string& code_like, bool use_string) {
  nlohmann::json args = nlohmann::json::object();
  if (use_string) {
    args["code"] = code_like;
  } else {
    args["code"] = nlohmann::json::array({code_like});
  }

  const absl::Status status = ValidateRunJsArgs(args);
  EXPECT_EQ(status.ok(), use_string && code_like.size() <= 256 * 1024);
}

void RunSmallScriptDoesNotCrash(const std::string& script) {
  if (script.size() > 4096) return;

  absl::StatusOr<nlohmann::json> result = RunJsForJson(script);
  if (!result.ok()) {
    EXPECT_NE(result.status().code(), absl::StatusCode::kOk);
  }
}

void ExecuteRunJsArgsDoesNotCrash(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = nlohmann::json{{"code", raw_args.substr(0, 4096)}};
  }
  if (args.is_object() && args.contains("code") && args["code"].is_string() &&
      args["code"].get<std::string>().size() > 4096) {
    args["code"] = args["code"].get<std::string>().substr(0, 4096);
  }

  (void)ExecuteRunJsArgs(args);
}

FUZZ_TEST(RunJsFuzzTest, ValidateRunJsArgsDeterministic);
FUZZ_TEST(RunJsFuzzTest, RunJsArgsRequireCodeString)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());
FUZZ_TEST(RunJsFuzzTest, RunSmallScriptDoesNotCrash);
FUZZ_TEST(RunJsFuzzTest, ExecuteRunJsArgsDoesNotCrash);

}  // namespace
}  // namespace slop