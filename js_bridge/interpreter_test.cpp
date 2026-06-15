
#include "js_bridge/interpreter.h"

#include <algorithm>
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

TEST(JsToolLibraryTest, HelpListsRevivedHelpers) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return tools.help();");

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE((*result)["tools"].is_array());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "read_file"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "grep"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "write_file"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "patch_tool"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "execute_bash"),
            (*result)["tools"].end());
}

TEST(JsToolLibraryTest, ReadFileValidatesPathBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.read_file({});",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "read_file requires string field path"));
}

TEST(JsToolLibraryTest, GrepValidatesPatternBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.grep({ path: '.' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "grep requires string field pattern"));
}

TEST(JsToolLibraryTest, WriteFileValidatesContentBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.write_file({ path: 'file.txt' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "write_file requires string field content"));
}

TEST(JsToolLibraryTest, PatchToolValidatesDryRunBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.patch_tool({ path: 'file.txt', unified_diff: 'diff', ignore_whitespace: false });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "patch_tool requires boolean field dry_run"));
}

TEST(JsToolLibraryTest, ExecuteBashValidatesAllowNonzeroExitBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.execute_bash({ cwd: '.', command: 'true' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(),
                                "execute_bash requires boolean field allow_nonzero_exit"));
}

TEST(JsToolLibraryTest, ReadFileCallsHostTool) {
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.read_file({ path: 'file.txt' });",
                   [](const std::string& name, const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     EXPECT_EQ(name, "read_file");
                     EXPECT_EQ(args, nlohmann::json({{"path", "file.txt"}}));
                     return std::string("contents");
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, "contents");
}

TEST(JsToolLibraryTest, SideEffectHelpersCallHostTools) {
  std::vector<std::string> called_tools;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson(R"js(
const write = tools.write_file({ path: 'file.txt', content: 'contents' });
const patch = tools.patch_tool({
  path: 'file.txt',
  unified_diff: '--- a/file.txt\n+++ b/file.txt\n',
  dry_run: true,
  ignore_whitespace: false
});
const shell = tools.execute_bash({
  cwd: '.',
  command: 'true',
  allow_nonzero_exit: false
});
return { write, patch, shell };
)js",
                   [&called_tools](const std::string& name,
                                   const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     called_tools.push_back(name);
                     EXPECT_TRUE(args.is_object());
                     return name;
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(called_tools, std::vector<std::string>({"write_file", "patch_tool", "execute_bash"}));
  EXPECT_EQ((*result)["write"], "write_file");
  EXPECT_EQ((*result)["patch"], "patch_tool");
  EXPECT_EQ((*result)["shell"], "execute_bash");
}

}  // namespace
}  // namespace slop