#include "core/tool_dispatcher.h"

#include <chrono>
#include <thread>

#include "gtest/gtest.h"

namespace slop {
namespace {

TEST(ToolDispatcherTest, ParallelExecution) {
  int call_count = 0;
  absl::Mutex mu;
  
  auto executor_func = [&](const std::string& name, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> /*cancellation*/) {
    {
      absl::MutexLock lock(&mu);
      call_count++;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return absl::StatusOr<std::string>("Success: " + name);
  };

  ToolDispatcher dispatcher(executor_func, 4);
  
  std::vector<ToolDispatcher::Call> calls = {
    {"1", "tool1", {}},
    {"2", "tool2", {}},
    {"3", "tool3", {}},
    {"4", "tool4", {}}
  };

  auto start = std::chrono::steady_clock::now();
  auto results = dispatcher.Dispatch(calls, nullptr);
  auto end = std::chrono::steady_clock::now();

  EXPECT_EQ(results.size(), 4);
  EXPECT_EQ(call_count, 4);
  
  // Should take around 100ms because they run in parallel, not 400ms.
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  EXPECT_LT(duration, 350); 

  EXPECT_EQ(results[0].id, "1");
  EXPECT_EQ(results[0].output.value(), "Success: tool1");
  EXPECT_EQ(results[3].id, "4");
  EXPECT_EQ(results[3].output.value(), "Success: tool4");
}

TEST(ToolDispatcherTest, Cancellation) {
  auto executor_func = [&](const std::string& /*name*/, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> cancellation) {
    for (int i = 0; i < 10; ++i) {
      if (cancellation && cancellation->IsCancelled()) {
        return absl::StatusOr<std::string>(absl::CancelledError("Cancelled"));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return absl::StatusOr<std::string>("Success");
  };

  ToolDispatcher dispatcher(executor_func, 4);
  auto cancellation = std::make_shared<CancellationRequest>();

  std::vector<ToolDispatcher::Call> calls = {
    {"1", "tool1", {}}
  };

  std::thread cancel_thread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancellation->Cancel();
  });

  auto results = dispatcher.Dispatch(calls, cancellation);
  cancel_thread.join();

  EXPECT_EQ(results.size(), 1);
  EXPECT_FALSE(results[0].output.ok());
  EXPECT_EQ(results[0].output.status().code(), absl::StatusCode::kCancelled);
}

TEST(ToolDispatcherTest, StressTest) {
  std::atomic<int> call_count{0};
  auto executor_func = [&](const std::string& /*name*/, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> /*cancellation*/) {
    call_count++;
    std::this_thread::yield();
    return absl::StatusOr<std::string>("ok");
  };

  ToolDispatcher dispatcher(executor_func, 8);
  
  for (int i = 0; i < 100; ++i) {
    std::vector<ToolDispatcher::Call> calls;
    for (int j = 0; j < 10; ++j) {
      calls.push_back({std::to_string(j), "tool", {}});
    }
    auto results = dispatcher.Dispatch(calls, nullptr);
    EXPECT_EQ(results.size(), 10);
  }
  
  EXPECT_EQ(call_count.load(), 1000);
}

TEST(ToolDispatcherTest, RapidChurnCancellation) {
  auto executor_func = [&](const std::string& /*name*/, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> cancellation) {
    while (!cancellation->IsCancelled()) {
      std::this_thread::yield();
    }
    return absl::StatusOr<std::string>(absl::CancelledError("cancelled"));
  };

  ToolDispatcher dispatcher(executor_func, 4);

  for (int i = 0; i < 50; ++i) {
    auto cancellation = std::make_shared<CancellationRequest>();
    std::vector<ToolDispatcher::Call> calls = {{"1", "long_job", {}}};
    
    std::thread t([&] {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      cancellation->Cancel();
    });
    
    auto results = dispatcher.Dispatch(calls, cancellation);
    t.join();
    
    EXPECT_FALSE(results[0].output.ok());
  }
}

}  // namespace
}  // namespace slop
