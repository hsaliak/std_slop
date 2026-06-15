
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
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

void BridgeInputDoesNotCrash(const std::string& tool_name, const std::string& raw_args) {
  const std::string escaped_name = json_dump(nlohmann::json(tool_name));
  const std::string script = "return call_tool(" + escaped_name + ", " + raw_args + ");";

  absl::StatusOr<nlohmann::json> result =
      RunJsForJson(script, [](const std::string& name, const nlohmann::json& args) -> absl::StatusOr<std::string> {
        if (name.empty()) {
          return absl::InvalidArgumentError("empty tool name");
        }
        return json_dump(nlohmann::json({{"name", name}, {"arg_type", args.type_name()}}));
      });

  if (!result.ok()) {
    EXPECT_NE(result.status().code(), absl::StatusCode::kOk);
  }
}

void HelperValidationRejectsMalformedArgs(const std::string& helper, const std::string& raw_args) {
  if (helper != "read_file" && helper != "grep" && helper != "list_directory" && helper != "llm_query") {
    return;
  }
  const std::string script = "return tools." + helper + "(" + raw_args + ");";
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson(script, [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
        called = true;
        return std::string("called");
      });

  if (!result.ok()) {
    EXPECT_FALSE(called);
    EXPECT_NE(result.status().code(), absl::StatusCode::kOk);
  }
}

FUZZ_TEST(RunJsFuzzTest, ValidateRunJsArgsDeterministic);
FUZZ_TEST(RunJsFuzzTest, RunJsArgsRequireCodeString)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());
FUZZ_TEST(RunJsFuzzTest, RunSmallScriptDoesNotCrash);
FUZZ_TEST(RunJsFuzzTest, ExecuteRunJsArgsDoesNotCrash);
FUZZ_TEST(RunJsFuzzTest, BridgeInputDoesNotCrash)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());
FUZZ_TEST(RunJsFuzzTest, HelperValidationRejectsMalformedArgs)
    .WithDomains(fuzztest::ElementOf<std::string>({"read_file", "grep", "list_directory", "llm_query"}),
                 fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop