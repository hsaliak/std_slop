#include <gtest/gtest.h>
#include "core/tool_executor.h"
#include "core/database.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "absl/strings/match.h"

namespace slop {

class JsIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    executor_ = ToolExecutor::Create(&db_).value();
    executor_->SetSessionId("test_session");
    setenv("SLOP_USE_JS", "1", 1);
  }

  void TearDown() override {
    unsetenv("SLOP_USE_JS");
  }

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

}  // namespace slop

