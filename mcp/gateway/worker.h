#ifndef SLOP_MCP_GATEWAY_WORKER_H_
#define SLOP_MCP_GATEWAY_WORKER_H_

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "js_runtime/runtime.h"

namespace slop::mcp::gateway {

struct WorkerCallRequest {
  std::uint64_t run_id;
  std::uint64_t call_id;
  std::string server;
  std::string tool;
  nlohmann::json arguments;
};

absl::StatusOr<WorkerCallRequest> ParseWorkerCallRequest(const nlohmann::json& message, std::uint64_t expected_run_id);

absl::StatusOr<nlohmann::json> ExecuteInWorker(const std::string& executable_path, const std::string& code,
                                               const nlohmann::json& input, js_runtime::AsyncToolBroker* broker,
                                               js_runtime::RuntimeOptions options, std::uint64_t trace_id = 0);

int RunWorkerMode(int ipc_fd);

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_WORKER_H_
