#include <gtest/gtest.h>
#include "core/tool_executor.h"
#include "core/database.h"
#include <cstdlib>

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
  args["script"] = "1 + 1";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("Return Value: 2") != std::string::npos);
}

TEST_F(JsIntegrationTest, JsPrint) {
  nlohmann::json args;
  args["script"] = "print('Hello JS'); 'done'";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("Hello JS") != std::string::npos);
}

TEST_F(JsIntegrationTest, JsToolCall) {
  // Test calling a native tool from JS
  nlohmann::json args;
  args["script"] = "tools.read_file({path: 'non_existent.txt'})";
  auto res = executor_->Execute("run_js", args);
  // Should fail with an error message from the tool
  ASSERT_FALSE(res.ok());
  EXPECT_TRUE(res.status().message().find("Could not open file") != std::string::npos);
}

TEST_F(JsIntegrationTest, JsPreamble) {
  // Test that preamble is loaded and core helpers work
  nlohmann::json args;
  args["script"] = "core.dispatch_tool('query_db', {sql: 'SELECT 1'})";
  auto res = executor_->Execute("run_js", args);
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("1") != std::string::npos);
}

}  // namespace slop
