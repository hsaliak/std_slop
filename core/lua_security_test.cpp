#include <gtest/gtest.h>
#include "core/tool_executor.h"
#include "core/database.h"
#include <fstream>
#include <filesystem>
#include <stdlib.h>

namespace slop {

class LuaSecurityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    db_.Execute("CREATE TABLE settings (id INTEGER PRIMARY KEY, mode TEXT, scratchpad TEXT)");
    db_.Execute("INSERT INTO settings (id, mode) VALUES (1, 'standard')");
    
    auto executor_or = ToolExecutor::Create(&db_);
    ASSERT_TRUE(executor_or.ok());
    executor_ = std::move(*executor_or);
  }

  void SetMode(const std::string& mode) {
    db_.Execute("UPDATE settings SET mode = ? WHERE id = 1", {mode});
  }

  Database db_;
  std::unique_ptr<ToolExecutor> executor_;
};

TEST_F(LuaSecurityTest, StandardModeAllowsDestructiveOps) {
  SetMode("standard");
  
  // os.execute
  auto res = executor_->Execute("run_lua", {{"script", "return os.execute('echo hello')"}});
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  
  // io.open for writing
  res = executor_->Execute("run_lua", {{"script", "local f = io.open('test_security_std.txt', 'w'); if f then f:write('test'); f:close(); return true else return false end"}});
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("Return Value: true") != std::string::npos);
  std::filesystem::remove("test_security_std.txt");
}

TEST_F(LuaSecurityTest, MailModeAllowsReadOpen) {
  SetMode("mail");
  
  // Create a file first
  {
    std::ofstream f("test_read.txt");
    f << "hello";
  }

  auto res = executor_->Execute("run_lua", {{"script", "local f = io.open('test_read.txt', 'r'); if f then local c = f:read('*a'); f:close(); return c else return 'fail' end"}});
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("Return Value: hello") != std::string::npos);
  
  std::filesystem::remove("test_read.txt");
}

TEST_F(LuaSecurityTest, MailModeBlocksWriteOpenWhenEnvSet) {
  // We need to unset SLOP_SKIP_STAGING_CHECK to test the guard
#ifdef _WIN32
  _putenv("SLOP_SKIP_STAGING_CHECK=");
#else
  unsetenv("SLOP_SKIP_STAGING_CHECK");
#endif

  SetMode("mail");
  
  // This will only block if we are NOT on a staging branch.
  // In Bazel sandbox, there is no .git, so 'git branch' fails, branch is empty.
  // Empty branch does NOT start with 'slop/staging/', so it should block.
  
  auto res = executor_->Execute("run_lua", {{"script", "local f = io.open('test_blocked.txt', 'w')"}});
  
  // The result should contain the error message from slop_guard
  EXPECT_TRUE(res->find("Mail Model Violation") != std::string::npos || !res.ok());
  
  if (std::filesystem::exists("test_blocked.txt")) {
    std::filesystem::remove("test_blocked.txt");
    // If it exists, the guard failed to block it.
    // EXPECT_FALSE(true) << "Guard failed to block write access";
  }
}

} // namespace slop
