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

  ToolDispatcher dispatcher(executor_func);

  std::vector<ToolDispatcher::Call> calls = {
      {"1", "tool1", {}}, {"2", "tool2", {}}, {"3", "tool3", {}}, {"4", "tool4", {}}};

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

  ToolDispatcher dispatcher(executor_func);
  auto cancellation = std::make_shared<CancellationRequest>();

  std::vector<ToolDispatcher::Call> calls = {{"1", "tool1", {}}};

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

  ToolDispatcher dispatcher(executor_func);

  for (int i = 0; i < 100; ++i) {
    std::vector<ToolDispatcher::Call> calls;
    calls.reserve(10);
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

  ToolDispatcher dispatcher(executor_func);

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

TEST(ToolDispatcherTest, NestedCancellation) {
  ToolDispatcher* dispatcher_ptr = nullptr;

  auto executor_func = [&](const std::string& name, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
    if (name == "parent") {
      // Parent spawns a child
      ToolDispatcher::Call child_call = {"child_id", "child", {}};
      auto job = dispatcher_ptr->Submit(child_call, cancellation);
      auto res = job->Wait();
      return res;
    } if (name == "child") {
      // Child waits for cancellation
      for (int i = 0; i < 100; ++i) {
        if (cancellation && cancellation->IsCancelled()) {
          return absl::StatusOr<std::string>(absl::CancelledError("Child Cancelled"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return absl::StatusOr<std::string>("Child finished without cancellation");
    }
    return absl::StatusOr<std::string>("unknown");
  };

  ToolDispatcher dispatcher(executor_func);
  dispatcher_ptr = &dispatcher;

  auto cancellation = std::make_shared<CancellationRequest>();
  ToolDispatcher::Call parent_call = {"parent_id", "parent", {}};

  std::thread cancel_thread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancellation->Cancel();
  });

  auto job = dispatcher.Submit(parent_call, cancellation);
  auto result = job->Wait();
  cancel_thread.join();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kCancelled);
}

TEST(ToolDispatcherTest, ImmediateCancellation) {
  auto executor_func = [&](const std::string& /*name*/, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> /*cancellation*/) -> absl::StatusOr<std::string> {
    return absl::StatusOr<std::string>("should not run");
  };

  ToolDispatcher dispatcher(executor_func);
  auto cancellation = std::make_shared<CancellationRequest>();
  cancellation->Cancel();

  ToolDispatcher::Call call = {"id", "test", {}};
  auto job = dispatcher.Submit(call, cancellation);
  auto result = job->Wait();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kCancelled);
}

TEST(ToolDispatcherTest, ManyJobsCancellation) {
  std::atomic<int> run_count{0};
  auto executor_func = [&](const std::string& /*name*/, const nlohmann::json& /*args*/,
                           std::shared_ptr<CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
    run_count++;
    while (!cancellation->IsCancelled()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return absl::StatusOr<std::string>(absl::CancelledError("Cancelled"));
  };

  ToolDispatcher dispatcher(executor_func);
  auto cancellation = std::make_shared<CancellationRequest>();

  std::vector<std::shared_ptr<ToolJob>> jobs;
  jobs.reserve(50);
for (int i = 0; i < 50; ++i) {
    jobs.push_back(dispatcher.Submit({"id" + std::to_string(i), "test", {}}, cancellation));
  }

  cancellation->Cancel();

  for (auto& job : jobs) {
    auto result = job->Wait();
    EXPECT_FALSE(result.ok());
  }
}

TEST(ToolDispatcherTest, DeeplyNestedCancellation) {
  ToolDispatcher* dispatcher_ptr = nullptr;

  std::function<absl::StatusOr<std::string>(const std::string&, const nlohmann::json&,
                                            std::shared_ptr<CancellationRequest>)>
      executor_func = [&](const std::string& /*name*/, const nlohmann::json& args,
                          std::shared_ptr<CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
    int depth = args.value("depth", 0);
    if (depth > 0) {
      nlohmann::json child_args = {{"depth", depth - 1}};
      auto job = dispatcher_ptr->Submit({"child_" + std::to_string(depth), "nested", child_args}, cancellation);
      return job->Wait();
    }       while (!cancellation->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return absl::StatusOr<std::string>(absl::CancelledError("Leaf Cancelled"));
   
  };

  ToolDispatcher dispatcher(executor_func);
  dispatcher_ptr = &dispatcher;

  auto cancellation = std::make_shared<CancellationRequest>();
  auto job = dispatcher.Submit({"root", "nested", {{"depth", 3}}}, cancellation);

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  cancellation->Cancel();

  auto result = job->Wait();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kCancelled);
}

}  // namespace
}  // namespace slop
