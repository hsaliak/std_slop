
#include "absl/strings/match.h"

#include "core/database.h"
#include "core/tool_dispatcher.h"
#include "core/tool_executor.h"

#include <gtest/gtest.h>

namespace slop {

TEST(AutoLoadTest, PersistentFunctionsAreAutoLoaded) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  // 1. Insert a persistent function
  const std::string func_name = "test_auto_load";
  const std::string func_code = "return function() { return 'Auto-load works!'; }";

  auto res_db = db.Query("INSERT INTO js_functions (name, code) VALUES (?, ?)", {func_name, func_code});
  ASSERT_TRUE(res_db.ok());

  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  // 2. Execute run_js and call the function directly
  std::string script = "return test_auto_load();";
  auto res = executor.Execute("run_js", {{"script", script}});

  ASSERT_TRUE(res.ok()) << res.status().message();
  EXPECT_TRUE(absl::StrContains(*res, "Auto-load works!"));
}

}  // namespace slop
