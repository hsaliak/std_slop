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

    unsetenv("SLOP_FORCE_BRANCH_NAME");
    unsetenv("SLOP_SKIP_STAGING_CHECK");


    ASSERT_TRUE(db_.Init(":memory:").ok());
    
    auto executor_or = ToolExecutor::Create(&db_);
    ASSERT_TRUE(executor_or.ok());
    executor_ = std::move(*executor_or);
  }

  void SetMode(const std::string& mode) {
    ASSERT_TRUE(db_.Execute("UPDATE settings SET mode = ? WHERE id = 1", {mode}).ok());
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
  res = executor_->Execute("run_lua", {{"script", "local f, err = io.open('test_security_std.txt', 'w'); if f then f:write('test'); f:close(); return 'true' else return 'err: ' .. tostring(err) end"}});
  ASSERT_TRUE(res.ok()) << res.status().ToString();
  EXPECT_TRUE(res->find("Return Value: true") != std::string::npos) << "Result: " << *res;
  
  if (std::filesystem::exists("test_security_std.txt")) {
    std::filesystem::remove("test_security_std.txt");
  }
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
  SetMode("mail");
  

  setenv("SLOP_FORCE_BRANCH_NAME", "main", 1);


  auto res = executor_->Execute("run_lua", {{"script", "local f = io.open('test_blocked.txt', 'w')"}});
  
  // The executor should return an error status because the Lua script failed
  EXPECT_FALSE(res.ok());
  EXPECT_TRUE(res.status().message().find("Mail Model Violation") != std::string::npos 
              || res.status().message().find("Destructive operations are only allowed") != std::string::npos) 
              << "Status: " << res.status().ToString();
  
  if (std::filesystem::exists("test_blocked.txt")) {
    std::filesystem::remove("test_blocked.txt");
    FAIL() << "File was created despite security guard";
  }
}

} // namespace slop
