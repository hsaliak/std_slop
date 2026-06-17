
#include "js_bridge/interpreter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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

TEST(RunJsArgsTest, ExecuteRunJsArgsExposesInputGlobal) {
  const nlohmann::json input = {
      {"message", "hello"},
      {"nested", {{"value", 42}}},
      {"list", nlohmann::json::array({"a", "b"})},
  };
  absl::StatusOr<nlohmann::json> result =
      ExecuteRunJsArgs(nlohmann::json{{"code", "return input;"}, {"input", input}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, input);
}

TEST(RunJsArgsTest, ExecuteRunJsArgsDefaultsInputToObject) {
  absl::StatusOr<nlohmann::json> result = ExecuteRunJsArgs(nlohmann::json{{"code", "return input;"}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json::object());
}

TEST(RunJsArgsTest, InputCarriesTextThatWouldBreakJsLiterals) {
  std::string hostile_text = "backtick ";
  hostile_text.push_back('`');
  hostile_text += " and template ${value} and quote ";
  hostile_text.push_back('\"');
  hostile_text += " and slash ";
  hostile_text.push_back('\\');
  hostile_text += "\nmarkdown fence ```cpp\nstd::string s = raw;\n```\n";
  hostile_text += "python triple quotes ";
  hostile_text += "\"\"\"";
  hostile_text += " and regex /a\\/b/ and trailing backslash ";
  hostile_text.push_back('\\');
  const nlohmann::json input = {{"old", hostile_text}, {"replacement", hostile_text + "\nreplacement"}};
  absl::StatusOr<nlohmann::json> result = ExecuteRunJsArgs(
      nlohmann::json{{"code", "return { old: input.old, replacement: input.replacement };"}, {"input", input}});

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)["old"], input["old"]);
  EXPECT_EQ((*result)["replacement"], input["replacement"]);
}

TEST(RunJsArgsTest, InputSupportsQuoteSafeEditPayload) {
  const std::string old_text = "const tricky = `template ${value}`;\nregex = /a\\/b/;\n";
  const std::string new_text = "const tricky = String.raw`template ${value}`;\nregex = /c\\/d/;\n";
  const nlohmann::json input = {
      {"edit", {{"path", "quoted.txt"},
                 {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", old_text}, {"text", new_text}}})}}},
      {"after", {{"path", "quoted.txt"}}},
  };
  std::vector<nlohmann::json> edit_calls;
  absl::StatusOr<nlohmann::json> result = ExecuteRunJsArgs(
      nlohmann::json{{"code", "const edit = tools.edit_tool('edit'); const after = tools.read_file(input.after); return { edit, after };"},
                     {"input", input}},
      [&edit_calls](const std::string& name, const nlohmann::json& args) -> absl::StatusOr<std::string> {
        if (name == "edit_tool") {
          edit_calls.push_back(args);
          return std::string(R"({"edits":1,"ok":true})");
        }
        if (name == "read_file") {
          return std::string(R"({"content":"updated"})");
        }
        return absl::InvalidArgumentError("unexpected tool: " + name);
      });

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_EQ(edit_calls.size(), 1);
  EXPECT_EQ(edit_calls[0], input["edit"]);
  EXPECT_EQ((*result)["edit"]["edits"], 1);
  EXPECT_EQ((*result)["after"]["content"], "updated");
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
  EXPECT_TRUE(absl::StrContains(result.status().message(), "echo args must be an object"));
}

TEST(JsToolLibraryTest, HelpListsRevivedHelpers) {
  absl::StatusOr<nlohmann::json> result = RunJsForJson("return tools.help();");

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE((*result)["tools"].is_array());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "read_file"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "grep"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "write_file"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "edit_tool"), (*result)["tools"].end());
  EXPECT_NE(std::find((*result)["tools"].begin(), (*result)["tools"].end(), "execute_bash"),
            (*result)["tools"].end());
}


TEST(JsToolLibraryTest, PersistFunctionValidatesRequiredFieldsBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.persist_function({ name: 'missingCode' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "persist_function requires string field code"));
}

TEST(JsToolLibraryTest, HelpIncludesPersistedGlobalsFromDatabase) {
  bool queried_functions = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.help();",
                   [&queried_functions](const std::string& name,
                                         const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     if (name != "query_db") return absl::NotFoundError("unexpected tool");
                     const std::string sql = args.value("sql", "");
                     if (absl::StrContains(sql, "js_functions")) {
                       queried_functions = true;
                       return std::string(R"([{"name":"tripleValue","description":"Return triple","json_schema":""}])");
                     }
                     return std::string(R"([{"name":"read_file","description":"Read file","json_schema":"{}","is_enabled":1,"is_top_level":0,"is_run_js_callable":1}])");
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(queried_functions);
  ASSERT_TRUE((*result)["persisted_globals"].is_array());
  ASSERT_EQ((*result)["persisted_globals"].size(), 1);
  EXPECT_EQ((*result)["persisted_globals"][0]["name"], "tripleValue");
  EXPECT_EQ((*result)["persisted_globals"][0]["description"], "Return triple");
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

TEST(JsToolLibraryTest, WriteFileRejectsDirectArgsBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.write_file({ path: 'file.txt', content: 'contents' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "tools.write_file inside run_js only accepts a non-empty input key string"));
}

TEST(JsToolLibraryTest, WriteFileValidatesInputPayloadBeforeHostCall) {
  const nlohmann::json input = {{"file_write", {{"path", "file.txt"}}}};
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.write_file('file_write');", input,
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "write_file requires string field content"));
}

TEST(JsToolLibraryTest, EditToolRejectsDirectArgsBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.edit_tool({ path: 'file.txt', edits: [] });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "tools.edit_tool inside run_js only accepts a non-empty input key string"));
}

TEST(JsToolLibraryTest, EditToolValidatesInputPayloadBeforeHostCall) {
  const nlohmann::json input = {{"file_edit", {{"path", "file.txt"}, {"edits", "not an array"}}}};
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.edit_tool('file_edit');", input,
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "edit_tool.edits must be an array"));
}

TEST(JsToolLibraryTest, EditToolRejectsMissingInputPayloadBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.edit_tool('missing_edit');", nlohmann::json::object(),
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(), "could not find input.missing_edit"));
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
  const nlohmann::json input = {
      {"file_write", {{"path", "file.txt"}, {"content", "contents"}}},
      {"file_edit",
       {{"path", "file.txt"},
        {"edits", nlohmann::json::array({{{"op", "replace"}, {"find", "old"}, {"text", "new"}}})}}},
  };
  std::vector<std::string> called_tools;
  std::vector<nlohmann::json> called_args;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson(R"js(
const write = tools.write_file('file_write');
const edit = tools.edit_tool('file_edit');
const shell = tools.execute_bash({
  cwd: '.',
  command: 'true',
  allow_nonzero_exit: false
});
return { write, edit, shell };
)js",
                   input,
                   [&called_tools, &called_args](const std::string& name,
                                                 const nlohmann::json& args) -> absl::StatusOr<std::string> {
                     called_tools.push_back(name);
                     called_args.push_back(args);
                     EXPECT_TRUE(args.is_object());
                     return name;
                   });

  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(called_tools, std::vector<std::string>({"write_file", "edit_tool", "execute_bash"}));
  ASSERT_EQ(called_args.size(), 3);
  EXPECT_EQ(called_args[0], input["file_write"]);
  EXPECT_EQ(called_args[1], input["file_edit"]);
  EXPECT_EQ((*result)["write"], "write_file");
  EXPECT_EQ((*result)["edit"], "edit_tool");
  EXPECT_EQ((*result)["shell"], "execute_bash");
}

TEST(JsToolLibraryTest, PayloadToolsRejectDispatchBypassBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return tools.dispatch('edit_tool', { path: 'file.txt', edits: [] });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(),
                                "tools.edit_tool inside run_js only accepts an input key string"));
}

TEST(JsToolLibraryTest, PayloadToolsRejectCallToolBypassBeforeHostCall) {
  bool called = false;
  absl::StatusOr<nlohmann::json> result =
      RunJsForJson("return call_tool('write_file', { path: 'file.txt', content: 'contents' });",
                   [&called](const std::string&, const nlohmann::json&) -> absl::StatusOr<std::string> {
                     called = true;
                     return std::string("unreachable");
                   });

  ASSERT_FALSE(result.ok());
  EXPECT_FALSE(called);
  EXPECT_TRUE(absl::StrContains(result.status().message(),
                                "tools.write_file inside run_js only accepts an input key string"));
}

}  // namespace
}  // namespace slop
