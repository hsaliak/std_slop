#include "mcp/server/stdio.h"

#include <unistd.h>

#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "mcp/json_rpc.h"

namespace slop::mcp::server {
namespace {

Server MakeServer(int* calls) {
  ImplementationInfo identity;
  identity.name = "stdio-test";
  identity.version = "1";
  ToolRegistration tool;
  tool.definition.name = "echo";
  tool.definition.input_schema = {{"type", "object"},
                                  {"required", {"text"}},
                                  {"properties", {{"text", {{"type", "string"}}}}},
                                  {"additionalProperties", false}};
  tool.handler = [calls](const nlohmann::json& arguments) -> absl::StatusOr<ToolCallResult> {
    ++*calls;
    ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", json_get_or(arguments, "text", std::string{})}});
    return result;
  };
  return *Server::Create(identity, {tool});
}

nlohmann::json Request(const std::string& method = "tools/call") {
  nlohmann::json request = {{"jsonrpc", "2.0"},
                            {"id", "stdio-1"},
                            {"method", method},
                            {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
  if (method == "tools/call") {
    request["params"]["name"] = "echo";
    request["params"]["arguments"] = {{"text", "hello"}};
  }
  return request;
}

std::vector<nlohmann::json> Responses(const std::string& text) {
  if (!text.empty()) EXPECT_EQ(text.back(), '\n');
  std::istringstream stream(text);
  std::string line;
  std::vector<nlohmann::json> responses;
  while (std::getline(stream, line)) {
    auto response = ParseJsonRpcMessage(line);
    EXPECT_TRUE(response.ok()) << line;
    if (!response.ok()) continue;
    EXPECT_TRUE(ParseJsonRpcResponse(*response).ok());
    responses.push_back(*response);
  }
  return responses;
}

class TrackingBuffer : public std::stringbuf {
 public:
  int flushes = 0;
  bool fail_flush = false;
  int sync() override {
    ++flushes;
    return fail_flush ? -1 : std::stringbuf::sync();
  }
};

class FailedWriteBuffer : public std::streambuf {
 protected:
  std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
  int_type overflow(int_type) override { return traits_type::eof(); }
};

class FailedReadBuffer : public std::stringbuf {
 public:
  explicit FailedReadBuffer(const std::string& text) : std::stringbuf(text) {}
  std::istream* input = nullptr;

 protected:
  int_type underflow() override {
    const auto byte = std::stringbuf::underflow();
    if (byte == traits_type::eof() && input != nullptr) input->setstate(std::ios::badbit);
    return byte;
  }
};

TEST(StdioTest, MultipleMessagesAndNotifications) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  auto notification = Request();
  notification.erase("id");
  std::istringstream input(json_dump(Request("server/discover")) + "\n" + json_dump(notification) + "\n" +
                           json_dump(Request("tools/list")) + "\n" + json_dump(Request()) + "\n");
  TrackingBuffer buffer;
  std::ostream output(&buffer);
  std::ostringstream diagnostics;
  EXPECT_TRUE(RunStdio(server, input, output, diagnostics).ok());
  const auto responses = Responses(buffer.str());
  ASSERT_EQ(responses.size(), 3);
  for (const auto& response : responses) EXPECT_EQ(response["id"], "stdio-1");
  EXPECT_TRUE(responses[0]["result"].contains("supportedVersions"));
  EXPECT_EQ(responses[1]["result"]["tools"][0]["name"], "echo");
  EXPECT_EQ(responses[2]["result"]["content"][0]["text"], "hello");
  EXPECT_EQ(buffer.flushes, 3);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(diagnostics.str().empty());
}

TEST(StdioTest, ProtocolErrorsAreRepliesAndDoNotStopReading) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  auto wrong_version = Request();
  wrong_version["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-11-25";
  std::istringstream input("\n{\n[]\n" + json_dump(wrong_version) + "\n" + json_dump(Request()) + "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_TRUE(RunStdio(server, input, output, diagnostics).ok());
  const auto responses = Responses(output.str());
  ASSERT_EQ(responses.size(), 5);
  EXPECT_EQ(responses[0]["error"]["code"], -32700);
  EXPECT_EQ(responses[1]["error"]["code"], -32700);
  EXPECT_EQ(responses[2]["error"]["code"], -32600);
  EXPECT_EQ(responses[3]["error"]["code"], -32022);
  EXPECT_EQ(responses[4]["result"]["isError"], false);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(diagnostics.str().empty());
}

TEST(StdioTest, EscapedNewlinesAndCrLf) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  auto request = Request();
  request["params"]["arguments"]["text"] = "hello\nworld\r\n";
  std::istringstream input(json_dump(request) + "\r\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_TRUE(RunStdio(server, input, output, diagnostics).ok());
  const auto responses = Responses(output.str());
  ASSERT_EQ(responses.size(), 1);
  EXPECT_EQ(responses[0]["result"]["content"][0]["text"], "hello\nworld\r\n");
  EXPECT_EQ(calls, 1);
}

TEST(StdioTest, EofAndTruncatedInput) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  std::ostringstream output;
  std::ostringstream diagnostics;
  std::istringstream empty;
  EXPECT_TRUE(RunStdio(server, empty, output, diagnostics).ok());
  EXPECT_TRUE(output.str().empty());
  EXPECT_TRUE(diagnostics.str().empty());
  std::istringstream truncated(json_dump(Request()));
  EXPECT_EQ(RunStdio(server, truncated, output, diagnostics).code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  EXPECT_FALSE(diagnostics.str().empty());
  EXPECT_EQ(diagnostics.str().find("hello"), std::string::npos);
}

TEST(StdioTest, ExactLimitExcludesLfAndCountsCr) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  const auto raw = json_dump(Request());
  std::ostringstream output;
  std::ostringstream diagnostics;
  StdioOptions options;
  options.max_input_bytes = raw.size();
  std::istringstream input(raw + "\n");
  EXPECT_TRUE(RunStdio(server, input, output, diagnostics, options).ok());
  EXPECT_EQ(calls, 1);
  std::istringstream crlf(raw + "\r\n");
  EXPECT_EQ(RunStdio(server, crlf, output, diagnostics, options).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(calls, 1);
}

TEST(StdioTest, OversizedInputStopsWithoutDispatchOrDrain) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  const auto raw = json_dump(Request());
  std::istringstream input(raw + "\n" + raw + "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  StdioOptions options;
  options.max_input_bytes = raw.size() - 1;
  EXPECT_EQ(RunStdio(server, input, output, diagnostics, options).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  EXPECT_EQ(input.peek(), '\n');
  EXPECT_EQ(diagnostics.str().find("hello"), std::string::npos);
  std::istringstream long_input(std::string(10000, 'x'));
  options.max_input_bytes = 4;
  EXPECT_EQ(RunStdio(server, long_input, output, diagnostics, options).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(long_input.tellg(), std::streampos(5));
}

TEST(StdioTest, EnforcesDefaultInputLimit) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  EXPECT_EQ(StdioOptions{}.max_input_bytes, 1024 * 1024);
  std::istringstream input(std::string(StdioOptions{}.max_input_bytes + 1, 'x'));
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
}

TEST(StdioTest, ZeroLimitFailsBeforeReading) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  std::istringstream input(json_dump(Request()) + "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  StdioOptions options;
  options.max_input_bytes = 0;
  EXPECT_EQ(RunStdio(server, input, output, diagnostics, options).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(input.tellg(), std::streampos(0));
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
}

TEST(StdioTest, InputFailuresDoNotDispatchPartialMessages) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  FailedReadBuffer buffer(json_dump(Request()));
  std::istream input(&buffer);
  buffer.input = &input;
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kInternal);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  EXPECT_FALSE(diagnostics.str().empty());
  std::istringstream failed_input(json_dump(Request()) + "\n");
  failed_input.setstate(std::ios::failbit);
  EXPECT_EQ(RunStdio(server, failed_input, output, diagnostics).code(), absl::StatusCode::kInternal);
  EXPECT_EQ(calls, 0);
}

TEST(StdioTest, OutputAndFlushFailuresStopBeforeNextCall) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  const auto raw = json_dump(Request()) + "\n";
  std::ostringstream diagnostics;
  FailedWriteBuffer failed_buffer;
  std::ostream output(&failed_buffer);
  std::istringstream input(raw + raw);
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kInternal);
  EXPECT_EQ(calls, 1);
  std::istringstream unread(raw);
  EXPECT_EQ(RunStdio(server, unread, output, diagnostics).code(), absl::StatusCode::kInternal);
  EXPECT_EQ(unread.tellg(), std::streampos(0));
  EXPECT_EQ(calls, 1);
  TrackingBuffer flush_buffer;
  flush_buffer.fail_flush = true;
  std::ostream flush_output(&flush_buffer);
  std::istringstream flush_input(raw + raw);
  EXPECT_EQ(RunStdio(server, flush_input, flush_output, diagnostics).code(), absl::StatusCode::kInternal);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(flush_buffer.flushes, 1);
}

TEST(StdioTest, FailedDiagnosticsDoNotMaskOriginalError) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  std::istringstream input("unterminated");
  std::ostringstream output;
  std::ostringstream diagnostics;
  diagnostics.setstate(std::ios::badbit);
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kDataLoss);
  EXPECT_TRUE(output.str().empty());
  std::istringstream valid(json_dump(Request()) + "\n");
  EXPECT_TRUE(RunStdio(server, valid, output, diagnostics).ok());
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(Responses(output.str()).size(), 1);
}

TEST(StdioTest, RejectsAliasedBuffersAndExceptionMasks) {
  int calls = 0;
  const Server server = MakeServer(&calls);
  std::istringstream input(json_dump(Request()) + "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_EQ(RunStdio(server, input, output, output).code(), absl::StatusCode::kInvalidArgument);
  std::ostream alias(output.rdbuf());
  EXPECT_EQ(RunStdio(server, input, output, alias).code(), absl::StatusCode::kInvalidArgument);
  input.exceptions(std::ios::badbit);
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kInvalidArgument);
  input.exceptions(std::ios::goodbit);
  output.exceptions(std::ios::badbit);
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kInvalidArgument);
  output.exceptions(std::ios::goodbit);
  diagnostics.exceptions(std::ios::badbit);
  EXPECT_EQ(RunStdio(server, input, output, diagnostics).code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  EXPECT_TRUE(diagnostics.str().empty());
}

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() { Close(); }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }
  void Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
  }

 private:
  int fd_;
};

class ScopedStdin {
 public:
  ScopedStdin() : saved_(dup(STDIN_FILENO)) {}
  ~ScopedStdin() {
    if (saved_.get() >= 0) {
      EXPECT_EQ(dup2(saved_.get(), STDIN_FILENO), STDIN_FILENO);
      std::cin.clear();
    }
  }
  bool Redirect(int fd) { return saved_.get() >= 0 && dup2(fd, STDIN_FILENO) == STDIN_FILENO; }

 private:
  ScopedFd saved_;
};

TEST(StdioTest, ReadsRealPipeFromStdinWithoutATerminal) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  ScopedFd read_end(fds[0]);
  ScopedFd write_end(fds[1]);
  const std::string payload = json_dump(Request("server/discover")) + "\n" + json_dump(Request()) + "\n";
  ASSERT_EQ(write(write_end.get(), payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));
  write_end.Close();
  ScopedStdin saved_stdin;
  ASSERT_TRUE(saved_stdin.Redirect(read_end.get()));
  std::cin.clear();
  int calls = 0;
  const Server server = MakeServer(&calls);
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_TRUE(RunStdio(server, std::cin, output, diagnostics).ok());
  const auto responses = Responses(output.str());
  ASSERT_EQ(responses.size(), 2);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(diagnostics.str().empty());
}

}  // namespace
}  // namespace slop::mcp::server
