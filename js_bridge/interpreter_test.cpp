
#include "js_bridge/interpreter.h"

#include <string>
#include <utility>

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

TEST(JsInterpreterBridgeTest, CallToolReturnsParsedJsonResult) {
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return call_tool('echo', { text: 'hello' });",
                   [](const std::string& name, const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     EXPECT_EQ(name, "echo");
                     EXPECT_EQ(args, nlohmann::json({{"text", "hello"}}));
                     return std::string(R"({"ok":true,"text":"hello"})");
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)["ok"], true);
  EXPECT_EQ((*result)["text"], "hello");
}

TEST(JsInterpreterBridgeTest, ToolsProxyCallsBridge) {
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.fake_tool({ value: 7 });",
                   [](const std::string& name, const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     EXPECT_EQ(name, "fake_tool");
                     EXPECT_EQ(args, nlohmann::json({{"value", 7}}));
                     return std::string("done");
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, "done");
}

TEST(JsInterpreterBridgeTest, UnknownToolStatusBecomesJavaScriptError) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson(
      "return call_tool('missing', {});", [](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
        return absl::NotFoundError("missing tool");
      });

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "missing tool"));
}

TEST(JsInterpreterBridgeTest, CallToolRequiresObjectArgs) {
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return call_tool('echo', ['not-object']);",
                   [](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     ADD_FAILURE() << "tool caller should not run for invalid bridge args";
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "call_tool args must be an object"));
}

}  // namespace
}  // namespace slop