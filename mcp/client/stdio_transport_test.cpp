#include "mcp/client/stdio_transport.h"

#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"

namespace slop::mcp {

std::string g_test_binary;

int RunChild(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string mode = argv[2];
  if (mode == "exit") return 0;
  if (mode == "invalid") {
    std::cout << "not-json\n" << std::flush;
    ::pause();
    return 0;
  }
  if (mode == "oversized") {
    std::cout << std::string(33, 'x') << '\n' << std::flush;
    ::pause();
    return 0;
  }
  if (mode == "hang" || mode == "ignore-term") {
    if (mode == "ignore-term") std::signal(SIGTERM, SIG_IGN);
    while (true) ::pause();
  }
  if (mode == "close-input") {
    close(STDIN_FILENO);
    std::cout << R"({"jsonrpc":"2.0","method":"test/ready"})" << '\n' << std::flush;
    while (true) ::pause();
  }

  if (mode == "stderr") std::cerr << "child diagnostic\n" << std::flush;
  if (mode == "echo" || mode == "stderr") {
    std::vector<std::string> args;
    for (int i = 3; i < argc; ++i) args.emplace_back(argv[i]);
    const nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "test/argv"}, {"params", {{"args", args}}}};
    std::cout << json_dump(notification) << '\n' << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) std::cout << line << '\n' << std::flush;
    return 0;
  }
  return 2;
}

namespace {

StdioTransport MakeTransport(std::string mode, std::vector<std::string> extra_args = {},
                             size_t max_request_bytes = 1024 * 1024, size_t max_response_bytes = 4 * 1024 * 1024) {
  StdioTransportOptions options;
  options.command = g_test_binary;
  options.args = {"--stdio-transport-child", std::move(mode)};
  options.args.insert(options.args.end(), std::make_move_iterator(extra_args.begin()),
                      std::make_move_iterator(extra_args.end()));
  options.max_request_bytes = max_request_bytes;
  options.max_response_bytes = max_response_bytes;
  return StdioTransport(std::move(options));
}

TEST(StdioTransportTest, SendsAndReceivesJsonRpcFramesAndPreservesLiteralArguments) {
  StdioTransport transport = MakeTransport("echo", {"space ; $() *"});
  ASSERT_TRUE(transport.Start().ok());

  auto args_notification = transport.Receive(absl::Seconds(1));
  ASSERT_TRUE(args_notification.ok()) << args_notification.status();
  EXPECT_EQ(json_get_or(*args_notification, "method", std::string()), "test/argv");
  auto params = json_get<nlohmann::json>(*args_notification, "params");
  ASSERT_TRUE(params.has_value());
  auto args = json_get<nlohmann::json>(*params, "args");
  ASSERT_TRUE(args.has_value());
  EXPECT_EQ(*args, nlohmann::json::array({"space ; $() *"}));

  const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}};
  ASSERT_TRUE(transport.Send(request, absl::Seconds(5)).ok());
  auto response = transport.Receive(absl::Seconds(1));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(*response, request);
  EXPECT_TRUE(transport.Close().ok());
  EXPECT_EQ(transport.Send(request, absl::Seconds(5)).code(), absl::StatusCode::kFailedPrecondition);
}

TEST(StdioTransportTest, KeepsChildStderrOutOfProtocolStream) {
  StdioTransport transport = MakeTransport("stderr");
  ASSERT_TRUE(transport.Start().ok());
  auto notification = transport.Receive(absl::Seconds(1));
  ASSERT_TRUE(notification.ok()) << notification.status();
  EXPECT_EQ(json_get_or(*notification, "method", std::string()), "test/argv");
  EXPECT_TRUE(transport.Close().ok());
}

TEST(StdioTransportTest, RejectsOversizedRequestBeforeWriting) {
  StdioTransport transport = MakeTransport("echo", {}, 16);
  ASSERT_TRUE(transport.Start().ok());
  const absl::Status status = transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "a-long-method"}}, absl::Seconds(5));
  EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
  EXPECT_TRUE(transport.Close().ok());
}

TEST(StdioTransportTest, RejectsOversizedResponseAndClosesChild) {
  StdioTransport transport = MakeTransport("oversized", {}, 1024, 32);
  ASSERT_TRUE(transport.Start().ok());
  auto response = transport.Receive(absl::Seconds(1));
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.status().code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, absl::Seconds(5)).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(StdioTransportTest, RejectsMalformedResponseAndClosesChild) {
  StdioTransport transport = MakeTransport("invalid");
  ASSERT_TRUE(transport.Start().ok());
  auto response = transport.Receive(absl::Seconds(1));
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, absl::Seconds(5)).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(StdioTransportTest, ReturnsDeadlineExceededAndClosesChildOnTimeout) {
  StdioTransport transport = MakeTransport("hang");
  ASSERT_TRUE(transport.Start().ok());
  auto response = transport.Receive(absl::Milliseconds(20));
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.status().code(), absl::StatusCode::kDeadlineExceeded);
  EXPECT_EQ(transport.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, absl::Seconds(5)).code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(StdioTransportTest, EscalatesToKillWhenChildIgnoresTerm) {
  StdioTransport transport = MakeTransport("ignore-term");
  ASSERT_TRUE(transport.Start().ok());
  EXPECT_TRUE(transport.Close().ok());
}

TEST(StdioTransportTest, HandlesChildExitAndBrokenInputWithoutHostSigpipe) {
  StdioTransport exited = MakeTransport("exit");
  ASSERT_TRUE(exited.Start().ok());
  auto no_response = exited.Receive(absl::Seconds(1));
  ASSERT_FALSE(no_response.ok());
  EXPECT_EQ(no_response.status().code(), absl::StatusCode::kUnavailable);

  StdioTransport closed_input = MakeTransport("close-input");
  ASSERT_TRUE(closed_input.Start().ok());
  auto ready = closed_input.Receive(absl::Seconds(1));
  ASSERT_TRUE(ready.ok()) << ready.status();
  const absl::Status send_status = closed_input.Send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, absl::Seconds(5));
  EXPECT_EQ(send_status.code(), absl::StatusCode::kUnavailable);
}

TEST(StdioTransportTest, ValidatesOptionsAndLifecycle) {
  StdioTransportOptions options;
  StdioTransport transport(std::move(options));
  EXPECT_EQ(transport.Start().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(transport.Close().ok());
  EXPECT_EQ(transport.Start().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(StdioTransportTest, RejectsNullArgumentsAndMissingExecutable) {
  StdioTransportOptions invalid_argument_options;
  invalid_argument_options.command = "/bin/true";
  invalid_argument_options.args.emplace_back("before\0after", 12);
  StdioTransport invalid_argument(std::move(invalid_argument_options));
  EXPECT_EQ(invalid_argument.Start().code(), absl::StatusCode::kInvalidArgument);

  StdioTransportOptions missing_command_options;
  missing_command_options.command = "/path/that/does/not/exist/mcp-server";
  StdioTransport missing_command(std::move(missing_command_options));
  EXPECT_EQ(missing_command.Start().code(), absl::StatusCode::kUnavailable);
}

TEST(StdioTransportTest, RejectsInvalidOutboundJsonRpc) {
  StdioTransport transport = MakeTransport("echo");
  ASSERT_TRUE(transport.Start().ok());
  EXPECT_EQ(transport.Send({{"not_jsonrpc", true}}, absl::Seconds(5)).code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::mcp

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--stdio-transport-child") {
    return slop::mcp::RunChild(argc, argv);
  }
  std::error_code error;
  const std::filesystem::path executable = std::filesystem::canonical(argv[0], error);
  slop::mcp::g_test_binary = error ? argv[0] : executable.string();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
