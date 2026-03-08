#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/match.h"

#include "core/database.h"
#include "core/tool_executor.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace slop {

class JsIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    executor_ = ToolExecutor::Create(&db_).value();
    executor_->SetSessionId("test_session");
    setenv("SLOP_USE_JS", "1", 1);
  }

  void TearDown() override { unsetenv("SLOP_USE_JS"); }

  Database db_;
  std::unique_ptr<ToolExecutor> executor_;
};

TEST_F(JsIntegrationTest, RunJsBasic) {
  nlohmann::json args;
  args["script"] = "return 1 + 1;";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(absl::StrContains(*res, "2"));
}

TEST_F(JsIntegrationTest, TopLevelNativeToolContractsAreManifestBacked) {
  // 1) Discoverability smoke test: help should list targeted top-level contracts.
  auto help_res = executor_->Execute("help", nlohmann::json::object());
  ASSERT_TRUE(help_res.ok()) << help_res.status().ToString();

  auto help_envelope = nlohmann::json::parse(*help_res, nullptr, false);
  ASSERT_TRUE(help_envelope.is_object());
  ASSERT_TRUE(help_envelope.value("ok", false));
  ASSERT_TRUE(help_envelope.contains("result"));

  nlohmann::json help_json;
  if (help_envelope["result"].is_string()) {
    help_json = nlohmann::json::parse(help_envelope["result"].get<std::string>(), nullptr, false);
  } else {
    help_json = help_envelope["result"];
  }
  ASSERT_TRUE(help_json.is_array());

  auto has_name = [&help_json](const std::string& name) {
    for (const auto& row : help_json) {
      if (row.is_object() && row.value("name", std::string{}) == name) return true;
    }
    return false;
  };

  auto find_row = [&help_json](const std::string& name) -> nlohmann::json {
    for (const auto& row : help_json) {
      if (row.is_object() && row.value("name", std::string{}) == name) return row;
    }
    return nlohmann::json();
  };

  EXPECT_TRUE(has_name("ask_user"));
  EXPECT_TRUE(has_name("llm_query"));
  EXPECT_TRUE(has_name("query_db"));

  // 2) Execution smoke test for direct native tool call path.
  auto query_ok = executor_->Execute("query_db", {{"sql", "SELECT 1 AS v;"}});
  ASSERT_TRUE(query_ok.ok()) << query_ok.status().ToString();
  EXPECT_TRUE(absl::StrContains(*query_ok, "1"));

  // 3) Contract smoke test for llm_query entry: schema requires "query".
  auto llm_row = find_row("llm_query");
  ASSERT_TRUE(llm_row.is_object());
  ASSERT_TRUE(llm_row.contains("json_schema"));
  auto schema = llm_row["json_schema"];
  ASSERT_TRUE(schema.is_object());
  ASSERT_TRUE(schema.contains("required"));
  ASSERT_TRUE(schema["required"].is_array());

  bool requires_query = false;
  for (const auto& req : schema["required"]) {
    if (req.is_string() && req.get<std::string>() == "query") {
      requires_query = true;
      break;
    }
  }
  EXPECT_TRUE(requires_query);

  // 4) Regression: these top-level tools are static manifest contracts and should
  // not have JS shim code persisted in js_functions.
  auto rows_json = db_.Query(
      "SELECT name, code FROM js_functions WHERE name IN ('ask_user','llm_query','query_db') ORDER BY name");
  ASSERT_TRUE(rows_json.ok()) << rows_json.status().ToString();
  auto rows = nlohmann::json::parse(*rows_json, nullptr, false);
  ASSERT_TRUE(rows.is_array());
  ASSERT_EQ(rows.size(), 3);
  for (const auto& row : rows) {
    ASSERT_TRUE(row.is_object());
    ASSERT_TRUE(row.contains("code"));
    EXPECT_TRUE(row["code"].is_string());
    EXPECT_TRUE(row["code"].get<std::string>().empty());
  }
}

TEST_F(JsIntegrationTest, JsPrint) {
  nlohmann::json args;
  args["script"] = "print('Hello JS'); 'done'";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(absl::StrContains(*res, "Hello JS"));
}

TEST_F(JsIntegrationTest, JsToolCall) {
  // Test calling a native tool from JS
  nlohmann::json args;
  args["script"] = "tools.read_file({path: 'non_existent.txt'})";
  auto res = executor_->Execute("run_js", args);
  // Should fail with an error message from the tool
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(absl::StrContains(res.status().message(), "Could not open file"));
}

TEST_F(JsIntegrationTest, JsPreamble) {
  // Test that preamble is loaded and core helpers work
  nlohmann::json args;
  args["script"] = "return core.dispatch_tool('query_db', {sql: 'SELECT 1'});";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(absl::StrContains(*res, "1"));
}

TEST_F(JsIntegrationTest, PersistFunction) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "add_two",
      code: "return function(x) { return x + 2; }",
      test_args: [3],
      expected_result: 5
    });
    if (!success) throw new Error(msg);
    return globalThis.add_two(10).toString();
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "12"));
}

TEST_F(JsIntegrationTest, UseSkill) {
  // Setup a dummy skill in the DB
  ASSERT_TRUE(db_.Execute("INSERT INTO sessions (id, active_skills) VALUES ('test_session', '[]');").ok());
  ASSERT_TRUE(db_.Execute("INSERT INTO skills (name, system_prompt_patch) VALUES ('test_skill', 'patch');").ok());

  auto res = executor_->Execute("run_js", {{"script", R"(
    return tools.use_skill({name: "test_skill", action: "activate"});
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "activated"));

  // Verify it was added to the session
  auto db_res = db_.Query("SELECT active_skills FROM sessions WHERE id = ?;", {"test_session"});
  ASSERT_TRUE(db_res.ok());
  EXPECT_TRUE(absl::StrContains(*db_res, "test_skill"));
}

TEST_F(JsIntegrationTest, PersistFunctionWithoutExpectedResult) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "inc_no_expect",
      code: "return function(x) { return x + 1; }"
    });
    if (!success) throw new Error(msg);
    return globalThis.inc_no_expect(41).toString();
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "42"));
}

TEST_F(JsIntegrationTest, PersistFunctionInvalidNameRejected) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "bad-name",
      code: "return function() { return 1; }"
    });
    if (success) throw new Error("expected failure");
    return msg;
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "name must match"));
}

TEST_F(JsIntegrationTest, PersistFunctionObjectExpectedResult) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "obj_result",
      code: "return function() { return {a: 1, b: [2, 3]}; }",
      expected_result: {a: 1, b: [2, 3]}
    });
    if (!success) throw new Error(msg);
    return "ok";
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "ok"));
}

TEST_F(JsIntegrationTest, RunJsHelpListsEmbeddedTools) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    return tools.help({});
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "\"name\":\"help\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"name\":\"execute_bash\""));
  EXPECT_FALSE(absl::StrContains(*res, "\"name\":\"run_js\""));
}

TEST_F(JsIntegrationTest, RunJsHelpIncludesPersistedFunctionDynamically) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "dynamic_tool",
      code: "return function() { return 'ok'; }",
      description: "dynamic tool for help test",
      json_schema: {type: 'object', properties: {}, required: []}
    });
    if (!success) throw new Error(msg);
    return tools.help({});
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "\"name\":\"dynamic_tool\""));
  EXPECT_TRUE(absl::StrContains(*res, "\"source\":\"persisted\""));
}

TEST_F(JsIntegrationTest, PersistFunctionRejectsNameCollision) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "JSON",
      code: "return function() { return 'nope'; }"
    });
    if (success) throw new Error("expected collision rejection");
    return msg;
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "name conflicts with an existing tool or global"));
}

TEST_F(JsIntegrationTest, ListDirectoryIgnoresCommonDirsByDefault) {
  const std::string root = "list_dir_ignore_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root + "/src");
  std::filesystem::create_directories(root + "/.git");
  std::filesystem::create_directories(root + "/node_modules");
  {
    std::ofstream(root + "/src/app.txt") << "x";
    std::ofstream(root + "/.git/config") << "x";
    std::ofstream(root + "/node_modules/pkg.js") << "x";
  }

  auto res_default = executor_->Execute("list_directory", {{"path", root}, {"depth", 2}});
  ASSERT_TRUE(res_default.ok()) << res_default.status().message();
  EXPECT_TRUE(absl::StrContains(*res_default, "Directory: src/"));
  EXPECT_FALSE(absl::StrContains(*res_default, ".git/"));
  EXPECT_FALSE(absl::StrContains(*res_default, "node_modules/"));

  auto res_all = executor_->Execute("list_directory", {{"path", root}, {"depth", 2}, {"include_ignored", true}});
  ASSERT_TRUE(res_all.ok()) << res_all.status().message();
  EXPECT_TRUE(absl::StrContains(*res_all, "Directory: .git/"));
  EXPECT_TRUE(absl::StrContains(*res_all, "Directory: node_modules/"));

  std::filesystem::remove_all(root);
}

TEST_F(JsIntegrationTest, GrepIgnoresCommonDirsByDefault) {
  const std::string root = "grep_ignore_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root + "/src");
  std::filesystem::create_directories(root + "/node_modules");
  {
    std::ofstream(root + "/src/app.js") << "needle_ignore";
    std::ofstream(root + "/node_modules/lib.js") << "needle_ignore";
  }

  auto res_default = executor_->Execute("grep", {{"pattern", "needle_ignore"}, {"path", root}});
  ASSERT_TRUE(res_default.ok()) << res_default.status().message();
  EXPECT_TRUE(absl::StrContains(*res_default, "src/app.js"));
  EXPECT_FALSE(absl::StrContains(*res_default, "node_modules/lib.js"));

  auto res_all = executor_->Execute("grep", {{"pattern", "needle_ignore"}, {"path", root}, {"include_ignored", true}});
  ASSERT_TRUE(res_all.ok()) << res_all.status().message();
  EXPECT_TRUE(absl::StrContains(*res_all, "node_modules/lib.js"));

  std::filesystem::remove_all(root);
}

TEST_F(JsIntegrationTest, TopLevelAwait) {
  nlohmann::json args;
  args["script"] = "const res = await Promise.resolve(42); return res;";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(absl::StrContains(*res, "42"));
}

TEST_F(JsIntegrationTest, RunJsAccurateLineNumbers) {
  auto args = slop::json_parse(R"({"script": "let x = ;\n"})").value();
  auto res = executor_->Execute("run_js", args);
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(absl::StrContains(res.status().message(), "input.js:1"));
}

TEST_F(JsIntegrationTest, PersistFunctionSideEffectPrevention) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    globalThis.sideEffectTriggered = false;
    const [success, msg] = tools.persist_function({
      name: "side_effect_test",
      code: "globalThis.sideEffectTriggered = true; return function() { return true; }"
    });
    if (!success) throw new Error(msg);
    return globalThis.sideEffectTriggered;
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "false"));
}

TEST_F(JsIntegrationTest, PersistFunctionCompileOnlyValidation) {
  auto res = executor_->Execute("run_js", {{"script", R"(
    const [success, msg] = tools.persist_function({
      name: "bad_syntax_test",
      code: "return function() { let x = ; }"
    });
    if (success) throw new Error("Expected syntax error");
    return msg;
  )"}});
  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Syntax Error"));
}

TEST_F(JsIntegrationTest, GitGrepStructuredAndRawModes) {
  const std::string root = "git_grep_test";
  std::filesystem::create_directories(root);
  {
    std::ofstream(root + "/a.txt") << "alpha needle\n";
    std::ofstream(root + "/b.txt") << "beta\n";
  }

  auto structured = executor_->Execute("git_grep", {{"pattern", "needle"}, {"paths", nlohmann::json::array({root})}});
  ASSERT_TRUE(structured.ok()) << structured.status().message();
  auto structured_json = nlohmann::json::parse(*structured, nullptr, false);
  ASSERT_TRUE(structured_json.is_object());
  auto structured_payload = structured_json.contains("result") ? structured_json["result"] : structured_json;
  ASSERT_TRUE(structured_payload.is_object());
  ASSERT_TRUE(structured_payload.contains("ok"));
  EXPECT_TRUE(structured_payload.value("ok", false));
  EXPECT_EQ(structured_payload.value("format", std::string{}), "structured");
  EXPECT_EQ(structured_payload.value("exitCode", -1), 0);
  ASSERT_TRUE(structured_payload.contains("data"));
  ASSERT_TRUE(structured_payload["data"].is_array());
  EXPECT_FALSE(structured_payload["data"].empty());

  auto raw = executor_->Execute("git_grep",
                                {{"pattern", "needle"}, {"paths", nlohmann::json::array({root})}, {"format", "raw"}});
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  auto raw_json = nlohmann::json::parse(*raw, nullptr, false);
  ASSERT_TRUE(raw_json.is_object());
  auto raw_payload = raw_json.contains("result") ? raw_json["result"] : raw_json;
  ASSERT_TRUE(raw_payload.is_object());
  EXPECT_EQ(raw_payload.value("format", std::string{}), "raw");
  ASSERT_TRUE(raw_payload.contains("data"));
  EXPECT_TRUE(raw_payload["data"].is_string());
  EXPECT_TRUE(absl::StrContains(raw_payload["data"].get<std::string>(), "needle"));

  std::filesystem::remove_all(root);
}

TEST_F(JsIntegrationTest, GitGrepNoMatchAndNoIndexFallback) {
  const std::string root = "git_grep_noindex_test";
  std::filesystem::create_directories(root);
  std::ofstream(root + "/c.txt") << "gamma line\n";

  auto no_match = executor_->Execute(
      "git_grep", {{"pattern", "definitely_not_present_token"}, {"paths", nlohmann::json::array({"core"})}});
  ASSERT_TRUE(no_match.ok()) << no_match.status().message();
  auto no_match_json = nlohmann::json::parse(*no_match, nullptr, false);
  ASSERT_TRUE(no_match_json.is_object());
  auto no_match_payload = no_match_json.contains("result") ? no_match_json["result"] : no_match_json;
  ASSERT_TRUE(no_match_payload.is_object());
  EXPECT_EQ(no_match_payload.value("exitCode", -1), 1);
  ASSERT_TRUE(no_match_payload.contains("data"));
  ASSERT_TRUE(no_match_payload["data"].is_array());
  EXPECT_TRUE(no_match_payload["data"].empty());

  auto no_index =
      executor_->Execute("git_grep", {{"pattern", "gamma"}, {"cwd", root}, {"paths", nlohmann::json::array({"."})}});
  ASSERT_TRUE(no_index.ok()) << no_index.status().message();
  auto no_index_json = nlohmann::json::parse(*no_index, nullptr, false);
  ASSERT_TRUE(no_index_json.is_object());
  auto no_index_payload = no_index_json.contains("result") ? no_index_json["result"] : no_index_json;
  ASSERT_TRUE(no_index_payload.is_object());
  EXPECT_EQ(no_index_payload.value("mode", std::string{}), "no-index");
  EXPECT_EQ(no_index_payload.value("exitCode", -1), 0);

  std::filesystem::remove_all(root);
}

TEST_F(JsIntegrationTest, RootGitignoreSafeSubsetTranslationMatrix) {
  struct Case {
    std::string line;
    bool supported;
    bool is_dir;
    std::string expected_mapped_pattern;
  };

  const std::vector<Case> cases = {
      {"", false, false, ""},
      {"# comment", false, false, ""},
      {"build/", true, true, "build"},
      {"dist/", true, true, "dist"},
      {"*.log", true, false, "*.log"},
      {".env", true, false, ".env"},
      {"foo.log", true, false, "foo.log"},
      {"!keep.log", false, false, ""},
      {"**/gen", false, false, ""},
      {"a/b", false, false, ""},
      {"a/b/", false, false, ""},
  };

  auto trim = [](const std::string& in) -> std::string {
    const size_t start = in.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const size_t end = in.find_last_not_of(" \t\r\n");
    return in.substr(start, end - start + 1);
  };

  for (const auto& tc : cases) {
    const std::string line = trim(tc.line);
    bool supported = true;
    bool is_dir = false;
    std::string mapped;

    if (line.empty() || line[0] == '#') {
      supported = false;
    } else if (line[0] == '!' || absl::StrContains(line, "**")) {
      supported = false;
    } else if (!line.empty() && line.back() == '/') {
      const std::string dir = trim(line.substr(0, line.size() - 1));
      if (dir.empty() || absl::StrContains(dir, '/')) {
        supported = false;
      } else {
        is_dir = true;
        mapped = dir;
      }
    } else if (absl::StrContains(line, '/')) {
      supported = false;
    } else {
      is_dir = false;
      mapped = line;
    }

    EXPECT_EQ(supported, tc.supported) << "line='" << tc.line << "'";
    if (tc.supported) {
      EXPECT_EQ(is_dir, tc.is_dir) << "line='" << tc.line << "'";
      EXPECT_EQ(mapped, tc.expected_mapped_pattern) << "line='" << tc.line << "'";
    }
  }
}

}  // namespace slop







