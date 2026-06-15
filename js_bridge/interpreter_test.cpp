
#include "js_bridge/interpreter.h"

#include <string>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

TEST(JsInterpreterTest, RunJsonReturnsStructuredValue) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return { ok: true, value: 6 * 7 };");

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)["ok"], true);
  EXPECT_EQ((*result)["value"], 42);
}

TEST(JsInterpreterTest, RunJsonReturnsPrimitiveValue) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return 'hello';");

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, "hello");
}

TEST(JsInterpreterTest, SyntaxErrorReturnsInvalidArgument) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return (;");

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "JavaScript error"));
}

TEST(JsInterpreterTest, ThrownErrorReturnsInvalidArgument) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("throw new Error('boom');");

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "boom"));
}

TEST(JsInterpreterTest, UndefinedReturnIsRejected) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return undefined;");

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "not JSON-serializable"));
}

TEST(RunJsArgsTest, ValidatesCodeField) {
  EXPECT_TRUE(ValidateRunJsArgs(nlohmann::json{{"code", "return 1;"}}).ok());
  EXPECT_FALSE(ValidateRunJsArgs(nlohmann::json::array()).ok());
  EXPECT_FALSE(ValidateRunJsArgs(nlohmann::json::object()).ok());
  EXPECT_FALSE(ValidateRunJsArgs(nlohmann::json{{"code", 1}}).ok());
}

TEST(RunJsArgsTest, ExecuteRunJsArgsReturnsJson) {
  absl::StatusOr<nlohmann::json> result = ExecuteRunJsArgs(nlohmann::json{{"code", "return [1, 2, 3];"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json::array({1, 2, 3}));
}

}  // namespace
}  // namespace slop