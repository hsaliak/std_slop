#include "mcp/gateway/scheduler.h"

#include <algorithm>
#include <chrono>
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

#include "js_runtime/runtime.h"
#include "mcp/gateway/broker.h"
#include "mcp/protocol.h"

namespace slop::mcp::gateway {
namespace {

Tool GoTool() {
  Tool tool;
  tool.name = "go";
  tool.input_schema = {{"type", "object"}};
  return tool;
}

class CallBarrier {
 public:
  absl::Status ArriveAndWait() {
    absl::MutexLock lock(mutex_);
    ++arrived_;
    changed_.SignalAll();
    const absl::Time deadline = absl::Now() + absl::Seconds(2);
    while (arrived_ < 2) {
      if (changed_.WaitWithDeadline(&mutex_, deadline)) {
        return absl::DeadlineExceededError("calls did not overlap across server lanes");
      }
    }
    return absl::OkStatus();
  }

 private:
  absl::Mutex mutex_;
  absl::CondVar changed_;
  int arrived_ ABSL_GUARDED_BY(mutex_) = 0;
};

class BarrierClient final : public Client {
 public:
  BarrierClient(std::shared_ptr<CallBarrier> barrier, std::string alias)
      : barrier_(std::move(barrier)), alias_(std::move(alias)) {}
  ProtocolRevision revision() const override { return ProtocolRevision::k2026_07_28; }
  absl::StatusOr<std::vector<Tool>> ListTools() override { return std::vector<Tool>{GoTool()}; }
  absl::StatusOr<ToolCallResult> CallTool(const std::string&, const nlohmann::json&, absl::Duration) override {
    absl::Status status = barrier_->ArriveAndWait();
    if (!status.ok()) return status;
    ToolCallResult result;
    result.structured_content = nlohmann::json::object({{"server", alias_}});
    return result;
  }
  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string&, const nlohmann::json&, const nlohmann::json&,
                                                  absl::Duration) override {
    return absl::UnimplementedError("not used");
  }

 private:
  std::shared_ptr<CallBarrier> barrier_;
  std::string alias_;
};

class ProbeState {
 public:
  bool WaitForCallCount(int count, absl::Duration timeout) {
    absl::MutexLock lock(mutex_);
    const absl::Time deadline = absl::Now() + timeout;
    while (calls_ < count) {
      if (changed_.WaitWithDeadline(&mutex_, deadline)) return calls_ >= count;
    }
    return true;
  }

  void Enter() {
    absl::MutexLock lock(mutex_);
    ++calls_;
    ++active_;
    max_active_ = std::max(max_active_, active_);
    changed_.SignalAll();
    while (!released_) changed_.Wait(&mutex_);
    --active_;
  }

  void Release() {
    absl::MutexLock lock(mutex_);
    released_ = true;
    changed_.SignalAll();
  }

  int calls() const {
    absl::MutexLock lock(mutex_);
    return calls_;
  }

  int max_active() const {
    absl::MutexLock lock(mutex_);
    return max_active_;
  }

 private:
  mutable absl::Mutex mutex_;
  absl::CondVar changed_;
  int calls_ ABSL_GUARDED_BY(mutex_) = 0;
  int active_ ABSL_GUARDED_BY(mutex_) = 0;
  int max_active_ ABSL_GUARDED_BY(mutex_) = 0;
  bool released_ ABSL_GUARDED_BY(mutex_) = false;
};

class ProbeClient final : public Client {
 public:
  explicit ProbeClient(std::shared_ptr<ProbeState> state) : state_(std::move(state)) {}
  ProtocolRevision revision() const override { return ProtocolRevision::k2026_07_28; }
  absl::StatusOr<std::vector<Tool>> ListTools() override { return std::vector<Tool>{GoTool()}; }
  absl::StatusOr<ToolCallResult> CallTool(const std::string&, const nlohmann::json&, absl::Duration) override {
    state_->Enter();
    ToolCallResult result;
    result.structured_content = nlohmann::json::object({{"ok", true}});
    return result;
  }
  absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string&, const nlohmann::json&, const nlohmann::json&,
                                                  absl::Duration) override {
    return absl::UnimplementedError("not used");
  }

 private:
  std::shared_ptr<ProbeState> state_;
};

std::vector<DownstreamClient> MakeBarrierClients(const std::shared_ptr<CallBarrier>& barrier) {
  std::vector<DownstreamClient> clients;
  for (const std::string& alias : {"alpha", "beta"}) {
    clients.push_back({alias, {"go"}, std::make_unique<BarrierClient>(barrier, alias)});
  }
  return clients;
}

TEST(SchedulerTest, CallsIndependentServerLanesConcurrently) {
  auto barrier = std::make_shared<CallBarrier>();
  auto catalog = Catalog::Create(MakeBarrierClients(barrier));
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  auto broker = ParallelBroker::Create(std::move(*catalog));
  ASSERT_TRUE(broker.ok()) << broker.status();

  js_runtime::RuntimeOptions options;
  options.timeout = std::chrono::seconds(4);
  auto result = js_runtime::Run(
      "const values = await Promise.all([alpha.go({}), beta.go({})]); "
      "return values.map((value) => value.structuredContent.server);",
      nlohmann::json::object(), broker->get(), options);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json({"alpha", "beta"}));
}

TEST(SchedulerTest, SerializesCallsForOneClientLane) {
  auto state = std::make_shared<ProbeState>();
  std::vector<DownstreamClient> clients;
  clients.push_back({"only", {"go"}, std::make_unique<ProbeClient>(state)});
  auto catalog = Catalog::Create(std::move(clients));
  ASSERT_TRUE(catalog.ok()) << catalog.status();
  auto broker = ParallelBroker::Create(std::move(*catalog));
  ASSERT_TRUE(broker.ok()) << broker.status();

  std::optional<absl::StatusOr<nlohmann::json>> result;
  js_runtime::RuntimeOptions options;
  options.timeout = std::chrono::seconds(3);
  std::thread runner([&] {
    result.emplace(js_runtime::Run("await Promise.all([only.go({}), only.go({})]); return true;",
                                   nlohmann::json::object(), broker->get(), options));
  });
  const bool first_entered = state->WaitForCallCount(1, absl::Seconds(1));
  const bool second_entered_while_first_blocked = state->WaitForCallCount(2, absl::Milliseconds(100));
  state->Release();
  runner.join();

  ASSERT_TRUE(first_entered);
  EXPECT_FALSE(second_entered_while_first_blocked);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->ok()) << result->status();
  EXPECT_EQ(**result, true);
  EXPECT_EQ(state->calls(), 2);
  EXPECT_EQ(state->max_active(), 1);
}

}  // namespace
}  // namespace slop::mcp::gateway
