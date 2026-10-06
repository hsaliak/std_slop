#ifndef SLOP_JS_RUNTIME_RUNTIME_H_
#define SLOP_JS_RUNTIME_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace slop::js_runtime {

struct ToolRequest {
  std::uint64_t run_id;
  std::uint64_t id;
  std::chrono::steady_clock::time_point deadline;
  std::string server;
  std::string tool;
  nlohmann::json arguments;
};

struct ToolCompletion {
  std::uint64_t run_id;
  std::uint64_t id;
  bool ok;
  nlohmann::json result;
  std::string error;
  std::string error_category = "tool";
};

class AsyncToolBroker {
 public:
  virtual ~AsyncToolBroker() = default;
  virtual absl::Status Submit(const ToolRequest& request) = 0;
  virtual std::vector<ToolCompletion> TakeCompletions() = 0;
  virtual void WaitForCompletion(std::chrono::milliseconds duration) = 0;
  virtual void CancelPending() = 0;
  virtual nlohmann::json PublicCatalog() const { return nlohmann::json::array(); }
  virtual absl::StatusOr<nlohmann::json> Help(const std::string&, const std::string&) const {
    return nlohmann::json{{"servers", nlohmann::json::array()}, {"tools", nlohmann::json::array()}};
  }
};

struct RuntimeOptions {
  std::chrono::milliseconds timeout = std::chrono::seconds(30);
  std::size_t memory_limit_bytes = 64 * 1024 * 1024;
  std::size_t max_code_bytes = 256 * 1024;
  std::size_t max_output_bytes = 1024 * 1024;
  std::size_t max_pending_calls = 64;
  std::size_t max_jobs_per_turn = 256;
};

// Executes code as an async function. Input is exposed as globalThis.input.
// Scripts call tools through: await mcp.call(server, tool, arguments).
absl::StatusOr<nlohmann::json> Run(std::string code, const nlohmann::json& input, AsyncToolBroker* broker,
                                   RuntimeOptions options = RuntimeOptions());

}  // namespace slop::js_runtime

#endif  // SLOP_JS_RUNTIME_RUNTIME_H_
