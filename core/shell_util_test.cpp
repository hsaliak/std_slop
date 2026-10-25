#include "core/shell_util.h"

#include <filesystem>
#include <chrono>
#include <string>
#include <thread>

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

TEST(ShellUtilTest, RunCommandCancellation) {
  auto cancellation = std::make_shared<CancellationRequest>();

  // Start a long-running command in a separate thread
  std::atomic<bool> thread_finished{false};
  absl::StatusOr<CommandResult> res;
  std::thread t([&] {
    res = RunCommand("sleep 10 && echo 'should not see this'", cancellation);
    thread_finished = true;
  });

  // Give it a moment to start
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  cancellation->Cancel();
  t.join();

  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kCancelled);
}

TEST(ShellUtilTest, RunCommandWithInput) {
  auto res = RunCommand("cat", nullptr, "hello world");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "hello world");
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandNoHangOnInput) {
  // cat without input will hang if stdin is not closed/redirected.
  // Our implementation should provide an empty pipe which is closed,
  // effectively giving EOF to the child.
  auto res = RunCommand("cat");
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "");
  EXPECT_EQ(res->exit_code, 0);
}

TEST(ShellUtilTest, RunCommandTimeout) {
  auto res = RunCommand("sleep 10", nullptr, "", 1);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST(ShellUtilTest, RunCommandUsesChildWorkingDirectoryOnly) {
  const std::filesystem::path original_directory = std::filesystem::current_path();
  const std::filesystem::path expected_directory = std::filesystem::canonical("/tmp");
  auto result = RunCommand("pwd -P", nullptr, "", 0, "/tmp");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->stdout_out, expected_directory.string() + "\n");
  EXPECT_EQ(std::filesystem::current_path(), original_directory);
}

TEST(ShellUtilTest, RunCommandWorkingDirectoryFailureIsReported) {
  auto result = RunCommand("printf should-not-run", nullptr, "", 0, "/path/that/does/not/exist");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->exit_code, 1);
  EXPECT_NE(result->stderr_out.find("Failed to change working directory"), std::string::npos);
  EXPECT_TRUE(result->stdout_out.empty());
}

TEST(ShellUtilTest, RunCommandOutputLimitKillsCommand) {
  auto result = RunCommand("while :; do printf x; done", nullptr, "", 5, "", 1024);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(ShellUtilTest, RunCommandQuietProcessWithinTimeoutDoesNotFail) {
  auto res = RunCommand("sleep 1", nullptr, "", 5);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res->stdout_out, "");
  EXPECT_EQ(res->stderr_out, "");
  EXPECT_EQ(res->exit_code, 0);
}

}  // namespace slop
