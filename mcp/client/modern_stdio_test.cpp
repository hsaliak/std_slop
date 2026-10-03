#include "mcp/client/modern_stdio.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "core/json_utils.h"
#include "mcp/client/client.h"
#include "mcp/client/stdio_transport.h"
#include "mcp/client/transport.h"

namespace slop::mcp::v2026_07_28 {
namespace {

class FakeTransport final : public Transport {
 public:
  explicit FakeTransport(std::vector<nlohmann::json> responses = {}, bool delay_send = false)
      : responses_(std::move(responses)), delay_send_(delay_send) {}

  absl::Status Start() override {
    absl::MutexLock lock(mutex_);
    started_ = true;
    return absl::OkStatus();
  }

  absl::Status Send(const nlohmann::json& message) override {
    {
      absl::MutexLock lock(mutex_);
      if (!started_ || closed_) return absl::FailedPreconditionError("fake transport is not running");
      ++in_flight_;
      max_in_flight_ = std::max(max_in_flight_, in_flight_);
      last_request_id_ = json_get<nlohmann::json>(message, "id");
      sent_messages_.push_back(message);
    }
    if (delay_send_) std::this_thread::sleep_for(absl::ToChronoMilliseconds(absl::Milliseconds(10)));
    return absl::OkStatus();
  }

  absl::StatusOr<nlohmann::json> Receive(absl::Duration /*timeout*/) override {
    absl::MutexLock lock(mutex_);
    if (!receive_status_.ok()) return receive_status_;
    if (!responses_.empty()) {
      nlohmann::json response = std::move(responses_.front());
      responses_.erase(responses_.begin());
      --in_flight_;
      return response;
    }
    if (!last_request_id_) return absl::UnavailableError("fake transport has no request");
    --in_flight_;
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", *last_request_id_}, {"result", nlohmann::json::object()}};
  }

  absl::Status Close() override {
    absl::MutexLock lock(mutex_);
    closed_ = true;
    return absl::OkStatus();
  }

  void set_receive_status(absl::Status status) {
    absl::MutexLock lock(mutex_);
    receive_status_ = std::move(status);
  }
  int max_in_flight() const {
    absl::MutexLock lock(mutex_);
    return max_in_flight_;
  }
  bool closed() const {
    absl::MutexLock lock(mutex_);
    return closed_;
  }
  std::vector<nlohmann::json> sent_messages() const {
    absl::MutexLock lock(mutex_);
    return sent_messages_;
  }

 private:
  mutable absl::Mutex mutex_;
  std::vector<nlohmann::json> responses_;
  bool delay_send_;
  bool started_ = false;
  bool closed_ = false;
  int in_flight_ = 0;
  int max_in_flight_ = 0;
  std::optional<nlohmann::json> last_request_id_;
  absl::Status receive_status_;
  std::vector<nlohmann::json> sent_messages_;
};

Request MakeRequest(std::string id, std::string method = "tools/list") {
  Request request;
  request.id = std::move(id);
  request.method = std::move(method);
  request.context.client_info = {"test-client", "1.0", std::nullopt};
  return request;
}

TEST(ModernStdioExchangeTest, CollectsNotificationsAndMatchesResponseId) {
  std::vector<nlohmann::json> messages = {
      {{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", {{"progress", 1}}}},
      {{"jsonrpc", "2.0"}, {"id", "request-1"}, {"result", {{"tools", nlohmann::json::array()}}}},
  };
  auto transport = std::make_unique<FakeTransport>(std::move(messages));
  FakeTransport* raw = transport.get();
  StdioExchange exchange(std::move(transport));
  ASSERT_TRUE(exchange.Start().ok());

  auto result = exchange.Execute(MakeRequest("request-1"));

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->response.has_value());
  ASSERT_EQ(result->notifications.size(), 1);
  EXPECT_EQ(result->notifications[0].kind, ServerNotificationKind::kProgress);
  const auto sent = raw->sent_messages();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(json_get_or(sent[0], "method", std::string()), "tools/list");
  auto params = json_get<nlohmann::json>(sent[0], "params");
  ASSERT_TRUE(params.has_value());
  auto meta = json_get<nlohmann::json>(*params, "_meta");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(json_get_or(*meta, std::string(kProtocolVersionMetadata), std::string()), kModernProtocolVersion);
}

TEST(ModernStdioExchangeTest, RejectsWrongResponseIdAndClosesTransport) {
  auto transport = std::make_unique<FakeTransport>(std::vector<nlohmann::json>{
      {{"jsonrpc", "2.0"}, {"id", "other-request"}, {"result", nlohmann::json::object()}},
  });
  FakeTransport* raw = transport.get();
  StdioExchange exchange(std::move(transport));
  ASSERT_TRUE(exchange.Start().ok());

  auto result = exchange.Execute(MakeRequest("request-1"));

  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(raw->closed());
}

TEST(ModernStdioExchangeTest, MarksLostResponseAsAmbiguousWithoutReplay) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport* raw = transport.get();
  raw->set_receive_status(absl::DeadlineExceededError("response timeout"));
  StdioExchange exchange(std::move(transport));
  ASSERT_TRUE(exchange.Start().ok());

  Request request = MakeRequest("request-1", "tools/call");
  request.params = {{"name", "echo"}, {"arguments", {{"text", "value"}}}};
  auto result = exchange.Execute(request);

  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->failure.has_value());
  EXPECT_EQ(result->failure->execution, ExecutionCertainty::kMayHaveExecuted);
  EXPECT_EQ(raw->sent_messages().size(), 1);
}

TEST(ModernStdioExchangeTest, SerializesRequestsAndResponses) {
  auto transport = std::make_unique<FakeTransport>(std::vector<nlohmann::json>{}, true);
  FakeTransport* raw = transport.get();
  StdioExchange exchange(std::move(transport));
  ASSERT_TRUE(exchange.Start().ok());
  std::atomic<int> successes = 0;
  std::thread first([&] {
    if (exchange.Execute(MakeRequest("request-1")).ok()) ++successes;
  });
  std::thread second([&] {
    if (exchange.Execute(MakeRequest("request-2")).ok()) ++successes;
  });
  first.join();
  second.join();

  EXPECT_EQ(successes.load(), 2);
  EXPECT_EQ(raw->max_in_flight(), 1);
}

TEST(ModernStdioExchangeTest, RejectsServerRequestsAndCancelsSession) {
  auto transport = std::make_unique<FakeTransport>(std::vector<nlohmann::json>{
      {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "sampling/createMessage"}},
  });
  FakeTransport* raw = transport.get();
  StdioExchange exchange(std::move(transport));
  ASSERT_TRUE(exchange.Start().ok());
  EXPECT_EQ(exchange.Execute(MakeRequest("request-1")).status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(raw->closed());
}

TEST(ModernStdioClientTest, ConnectsToRepositoryEchoServerUsingModernProtocol) {
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const char* test_workspace = std::getenv("TEST_WORKSPACE");
  ASSERT_NE(test_srcdir, nullptr);
  ASSERT_NE(test_workspace, nullptr);

  StdioTransportOptions transport_options;
  transport_options.command = std::string(test_srcdir) + "/" + test_workspace + "/mcp/server/echo_server";
  ClientOptions options;
  options.client_info = {"stdio-test-client", "1.0", std::nullopt};
  auto client = slop::mcp::ConnectStdioMcp(std::move(transport_options), options);
  ASSERT_TRUE(client.ok()) << client.status();
  EXPECT_EQ((*client)->revision(), ProtocolRevision::k2026_07_28);

  auto tools = (*client)->ListTools();
  ASSERT_TRUE(tools.ok()) << tools.status();
  ASSERT_EQ(tools->size(), 1);
  EXPECT_EQ(tools->front().name, "echo");

  auto result = (*client)->CallTool("echo", {{"text", "stdio works"}});
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_TRUE(result->structured_content.has_value());
  EXPECT_EQ(*result->structured_content, nlohmann::json({{"text", "stdio works"}}));
}

TEST(ModernStdioClientTest, DoesNotStartClassicOnlyRegistration) {
  StdioTransportOptions transport_options;
  transport_options.command = "/path/that/does/not/exist/mcp-server";
  ClientOptions options;
  options.client_info = {"stdio-test-client", "1.0", std::nullopt};
  options.selection = SelectionPolicy::kClassicOnly;

  auto client = slop::mcp::ConnectStdioMcp(std::move(transport_options), options);

  EXPECT_EQ(client.status().code(), absl::StatusCode::kUnimplemented);
}

}  // namespace
}  // namespace slop::mcp::v2026_07_28
