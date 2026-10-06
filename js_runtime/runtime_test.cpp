#include "js_runtime/runtime.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop::js_runtime {
namespace {

class FakeBroker : public AsyncToolBroker {
 public:
  absl::Status Submit(const ToolRequest& request) override {
    ++submissions_;
    requests_.push_back(request);
    return absl::OkStatus();
  }

  std::vector<ToolCompletion> TakeCompletions() override {
    std::vector<ToolCompletion> result;
    result.swap(ready_);
    return result;
  }

  void WaitForCompletion(std::chrono::milliseconds) override {
    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
      ready_.push_back(
          {it->run_id, it->id, true, {{"server", it->server}, {"tool", it->tool}, {"args", it->arguments}}, ""});
    }
    requests_.clear();
  }

  void CancelPending() override { requests_.clear(); }
  int submissions() const { return submissions_; }

 private:
  int submissions_ = 0;
  std::vector<ToolRequest> requests_;
  std::vector<ToolCompletion> ready_;
};

TEST(RuntimeTest, RunsAsyncCodeWithJsonInput) {
  FakeBroker broker;
  auto result = slop::js_runtime::Run("return {answer: input.value + 1};", {{"value", 41}}, &broker);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json({{"answer", 42}}));
}

TEST(RuntimeTest, PreservesStringValuesInJsonConversion) {
  FakeBroker broker;
  auto result = slop::js_runtime::Run("return {text: 'hello', items: ['world']};", nlohmann::json::object(), &broker);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json({{"text", "hello"}, {"items", {"world"}}}));
}

TEST(RuntimeTest, AwaitsToolCall) {
  FakeBroker broker;
  auto result = slop::js_runtime::Run("const value = await mcp.call('echo', 'echo', {text: input.text}); return value;",
                                      {{"text", "hello"}}, &broker);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)["args"]["text"], "hello");
}

TEST(RuntimeTest, PromiseAllRetainsInputOrderForOutOfOrderCalls) {
  FakeBroker broker;
  auto result = slop::js_runtime::Run(
      "const values = await Promise.all([mcp.call('a', 'first', {}), mcp.call('b', 'second', {})]); "
      "return values.map((value) => value.tool);",
      nlohmann::json::object(), &broker);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, nlohmann::json({"first", "second"}));
}

TEST(RuntimeTest, RejectedToolCallCanBeCaught) {
  class RejectingBroker final : public AsyncToolBroker {
   public:
    absl::Status Submit(const ToolRequest& request) override {
      ready_.push_back({request.run_id, request.id, false, nullptr, "denied"});
      return absl::OkStatus();
    }
    std::vector<ToolCompletion> TakeCompletions() override {
      std::vector<ToolCompletion> ready;
      ready.swap(ready_);
      return ready;
    }
    void WaitForCompletion(std::chrono::milliseconds) override {}
    void CancelPending() override {}
    std::vector<ToolCompletion> ready_;
  } broker;

  auto result =
      slop::js_runtime::Run("try { await mcp.call('echo', 'denied', {}); } catch (error) { return error.message; }",
                            nlohmann::json::object(), &broker);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, "denied");
}

TEST(RuntimeTest, RejectsStalledPromise) {
  FakeBroker broker;
  auto result = slop::js_runtime::Run("return await new Promise(() => {});", nlohmann::json::object(), &broker);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("stalled_promise"), std::string::npos);
}

TEST(RuntimeTest, CapsTotalAdmittedCallsAcrossSequentialAwaits) {
  FakeBroker broker;
  RuntimeOptions options;
  options.max_pending_calls = 2;
  auto result = slop::js_runtime::Run(
      "await mcp.call('echo', 'one', {}); await mcp.call('echo', 'two', {}); await mcp.call('echo', 'three', {}); "
      "return 3;",
      nlohmann::json::object(), &broker, options);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(broker.submissions(), 2);
}

TEST(RuntimeTest, RejectsUnawaitedToolCalls) {
  FakeBroker broker;
  auto result =
      slop::js_runtime::Run("mcp.call('echo', 'fire-and-forget', {}); return 1;", nlohmann::json::object(), &broker);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("unawaited tool calls"), std::string::npos);
}

TEST(RuntimeTest, InterruptsInfiniteLoop) {
  FakeBroker broker;
  RuntimeOptions options;
  options.timeout = std::chrono::milliseconds(40);
  auto result = slop::js_runtime::Run("while (true) {}", nlohmann::json::object(), &broker, options);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST(RuntimeTest, RejectsUnsupportedResultValues) {
  FakeBroker broker;
  for (const std::string& code :
       {"return undefined;", "return 1n;", "return NaN;", "return Infinity;", "return {self: null};"}) {
    auto result = slop::js_runtime::Run(code, nlohmann::json::object(), &broker);
    if (code == "return {self: null};") {
      ASSERT_TRUE(result.ok()) << result.status();
    } else {
      EXPECT_FALSE(result.ok()) << code;
    }
  }
}

TEST(RuntimeTest, RejectsCyclesAndAccessorsWithoutInvokingGetters) {
  FakeBroker broker;
  auto cycle =
      slop::js_runtime::Run("const value = {}; value.self = value; return value;", nlohmann::json::object(), &broker);
  ASSERT_FALSE(cycle.ok());
  EXPECT_NE(cycle.status().message().find("cyclic"), std::string::npos);

  auto accessor = slop::js_runtime::Run(
      "const value = {}; Object.defineProperty(value, 'secret', {enumerable: true, get() { mcp.call('echo', 'getter', "
      "{}); return 1; }}); return value;",
      nlohmann::json::object(), &broker);
  ASSERT_FALSE(accessor.ok());
  EXPECT_EQ(broker.submissions(), 0);
  EXPECT_NE(accessor.status().message().find("accessor"), std::string::npos);
}

TEST(RuntimeTest, RejectsDeepAndExoticResults) {
  FakeBroker broker;
  auto proxy = slop::js_runtime::Run("return new Proxy({}, {});", nlohmann::json::object(), &broker);
  ASSERT_FALSE(proxy.ok());
  EXPECT_NE(proxy.status().message().find("Proxy"), std::string::npos);

  auto deep = slop::js_runtime::Run("let value = 0; for (let i = 0; i < 66; ++i) value = {value}; return value;",
                                    nlohmann::json::object(), &broker);
  ASSERT_FALSE(deep.ok());
  EXPECT_NE(deep.status().message().find("depth"), std::string::npos);
}

TEST(RuntimeTest, RejectsCodeAndCallLimits) {
  FakeBroker broker;
  RuntimeOptions options;
  options.max_code_bytes = 2;
  auto result = slop::js_runtime::Run("123", nlohmann::json::object(), &broker, options);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("maximum size"), std::string::npos);
}

}  // namespace
}  // namespace slop::js_runtime
