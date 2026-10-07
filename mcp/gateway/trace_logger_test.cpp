#include "mcp/gateway/trace_logger.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "mcp/gateway/result.h"

namespace slop::mcp::gateway {
namespace {

class TemporaryLog {
 public:
  TemporaryLog() {
    const int fd = mkstemp(path_template_);
    EXPECT_GE(fd, 0);
    if (fd >= 0) close(fd);
    path_ = path_template_;
  }

  ~TemporaryLog() { std::filesystem::remove(path_); }

  const std::string& path() const { return path_; }

 private:
  char path_template_[32] = "/tmp/slop-trace-XXXXXX";
  std::string path_;
};

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST(TraceLoggerTest, WritesReadableRunAndToolEventsWithPrivatePermissions) {
  TemporaryLog file;
  auto logger = TraceLogger::Open(file.path());
  ASSERT_TRUE(logger.ok()) << logger.status();

  const std::string code = "const result = await echo.echo({text: input.text});\nreturn result;";
  const nlohmann::json input = {{"text", "hello"}};
  const std::uint64_t trace_id = (*logger)->BeginRun(code, input);

  ToolCallResult raw;
  raw.content = {{{"type", "text"}, {"text", "hello"}}};
  raw.structured_content = {{"text", "hello"}};
  auto normalized = NormalizeToolResult(raw);
  ASSERT_TRUE(normalized.ok()) << normalized.status();

  (*logger)->LogToolCall(trace_id, 3, "echo", "echo", {{"text", "hello"}});
  (*logger)->LogToolResult(trace_id, 3, "echo", "echo", raw, normalized);
  (*logger)->FinishRun(trace_id, {{"result", {{"text", "hello"}}}});

  const std::string log = ReadFile(file.path());
  EXPECT_NE(log.find("event=RUN START"), std::string::npos);
  EXPECT_NE(log.find("input:\n{\n  \"text\": \"hello\"\n}"), std::string::npos);
  EXPECT_NE(log.find("code (terminal-safe; control bytes escaped):\n" + code), std::string::npos);
  EXPECT_NE(log.find("trace=" + std::to_string(trace_id) + " event=TOOL CALL 3"), std::string::npos);
  EXPECT_NE(log.find("arguments:\n{\n  \"text\": \"hello\"\n}"), std::string::npos);
  EXPECT_NE(log.find("event=TOOL RESULT 3"), std::string::npos);
  EXPECT_NE(log.find("raw downstream result:"), std::string::npos);
  EXPECT_NE(log.find("JavaScript-visible result:"), std::string::npos);
  EXPECT_NE(log.find("event=RUN SUCCESS"), std::string::npos);

  struct stat file_info {};
  ASSERT_EQ(stat(file.path().c_str(), &file_info), 0);
  EXPECT_EQ(file_info.st_mode & 0777, 0600);
}

TEST(TraceLoggerTest, EscapesTerminalControlBytesInCode) {
  TemporaryLog file;
  auto logger = TraceLogger::Open(file.path());
  ASSERT_TRUE(logger.ok()) << logger.status();

  const std::string code = "const color = '\x1b[31m';";
  (*logger)->BeginRun(code, nlohmann::json::object());

  const std::string log = ReadFile(file.path());
  EXPECT_NE(log.find(R"(const color = '\x1b[31m';)"), std::string::npos);
  EXPECT_EQ(log.find(static_cast<char>(0x1b)), std::string::npos);
}

TEST(TraceLoggerTest, AppendsNewRunRecords) {
  TemporaryLog file;
  {
    auto logger = TraceLogger::Open(file.path());
    ASSERT_TRUE(logger.ok()) << logger.status();
    (*logger)->BeginRun("return 1;", nlohmann::json::object());
  }
  {
    auto logger = TraceLogger::Open(file.path());
    ASSERT_TRUE(logger.ok()) << logger.status();
    (*logger)->BeginRun("return 2;", nlohmann::json::object());
  }
  const std::string log = ReadFile(file.path());
  EXPECT_NE(log.find("return 1;"), std::string::npos);
  EXPECT_NE(log.find("return 2;"), std::string::npos);
  EXPECT_EQ(log.find("event=RUN START", log.find("event=RUN START") + 1),
            log.rfind("event=RUN START"));
}

TEST(TraceLoggerTest, RejectsSymlinksAndNonAbsolutePaths) {
  TemporaryLog file;
  const std::string symlink_path = file.path() + "-link";
  ASSERT_EQ(::symlink(file.path().c_str(), symlink_path.c_str()), 0);
  EXPECT_FALSE(TraceLogger::Open(symlink_path).ok());
  std::filesystem::remove(symlink_path);
  EXPECT_FALSE(TraceLogger::Open("relative.log").ok());
  std::string nul_path = "/tmp/run_js.log";
  nul_path.push_back('\0');
  nul_path.append("suffix");
  EXPECT_FALSE(TraceLogger::Open(nul_path).ok());
}

}  // namespace
}  // namespace slop::mcp::gateway
