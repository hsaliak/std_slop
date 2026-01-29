#include "core/shell_util.h"

#include <string>

#include <gtest/gtest.h>

namespace slop {

TEST(ShellUtilTest, RunCommandStdoutOnly) {
  auto res = RunCommand("echo 'hello'");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "hello\n");
  EXPECT_EQ(res->stderr_out, "");
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandStderrOnly) {
  auto res = RunCommand("echo 'error' >&2");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "");
  EXPECT_EQ(res->stderr_out, "error\n");
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandBoth) {
  auto res = RunCommand("echo 'out' && echo 'err' >&2");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "out\n");
  EXPECT_EQ(res->stderr_out, "err\n");
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandExitCode) {
  auto res = RunCommand("exit 42");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->exit_code, 42);
}

TEST(ShellUtilTest, RunCommandLargeOutput) {
  // Generate large output to ensure multiple reads in the poll loop
  auto res = RunCommand("for i in $(seq 1 2000); do echo $i; done");
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->stdout_out.size() > 5000);
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandInvalidCommand) {
  auto res = RunCommand("nonexistent_command_12345");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->exit_code, 127);
  EXPECT_FALSE(res->stderr_out.empty());  // sh: nonexistent_command_12345: command not found
}

}  // namespace slop
