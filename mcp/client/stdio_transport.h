#ifndef SLOP_MCP_CLIENT_STDIO_TRANSPORT_H_
#define SLOP_MCP_CLIENT_STDIO_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "mcp/client/transport.h"

namespace slop::mcp {

struct StdioTransportOptions {
  // Executable path or program name resolved using the inherited PATH.
  std::string command;
  std::vector<std::string> args;
  // Maximum JSON bytes per frame, excluding the terminating LF.
  size_t max_request_bytes = 1024 * 1024;
  size_t max_response_bytes = 4 * 1024 * 1024;
  absl::Duration send_timeout = absl::Seconds(60);
};

// Owns one child process and exchanges newline-delimited JSON-RPC over its
// stdin/stdout. The command and arguments are passed directly to the process
// API; no shell is started. Child stderr inherits the parent's stderr.
class StdioTransport final : public Transport {
 public:
  explicit StdioTransport(StdioTransportOptions options);
  ~StdioTransport() override;

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  absl::Status Start() override;
  absl::Status Send(const nlohmann::json& message) override;
  absl::StatusOr<nlohmann::json> Receive(absl::Duration timeout) override;
  absl::Status Close() override;

 private:
  absl::Status WriteFrame(const std::string& frame);
  absl::Status FailAndClose(absl::Status status);
  absl::Status ReapChild();

  StdioTransportOptions options_;
  int input_fd_ = -1;
  int output_fd_ = -1;
  int64_t child_pid_ = -1;
  std::string buffered_output_;
  bool started_ = false;
  bool closed_ = false;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_CLIENT_STDIO_TRANSPORT_H_
