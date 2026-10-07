#ifndef SLOP_MCP_GATEWAY_TRACE_LOGGER_H_
#define SLOP_MCP_GATEWAY_TRACE_LOGGER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "nlohmann/json.hpp"

#include "mcp/gateway/result.h"
#include "mcp/types.h"

namespace slop::mcp::gateway {

class TraceLogger {
 public:
  static absl::StatusOr<std::shared_ptr<TraceLogger>> Open(const std::string& path);
  explicit TraceLogger(int fd) : fd_(fd) {}
  ~TraceLogger();

  TraceLogger(const TraceLogger&) = delete;
  TraceLogger& operator=(const TraceLogger&) = delete;

  std::uint64_t BeginRun(const std::string& code, const nlohmann::json& input);
  void LogToolCall(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                   const std::string& tool, const nlohmann::json& arguments);
  void LogToolResult(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                     const std::string& tool, const ToolCallResult& raw_result,
                     const absl::StatusOr<NormalizedToolResult>& normalized_result);
  void LogToolFailure(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                      const std::string& tool, const absl::Status& status);
  void FinishRun(std::uint64_t trace_id, const nlohmann::json& result);
  void FailRun(std::uint64_t trace_id, const absl::Status& status);

 private:
  absl::Status AppendRecord(std::string record);
  std::string EventHeader(const std::string& event, std::uint64_t trace_id) const;

  int fd_;
  std::atomic<std::uint64_t> next_trace_id_{1};
  absl::Mutex mutex_;
};

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_TRACE_LOGGER_H_
