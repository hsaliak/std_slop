#ifndef SLOP_MCP_CLIENT_MODERN_STDIO_H_
#define SLOP_MCP_CLIENT_MODERN_STDIO_H_

#include <cstddef>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "mcp/client/modern_exchange.h"
#include "mcp/client/transport.h"

namespace slop::mcp::v2026_07_28 {

struct StdioExchangeOptions {
  absl::Duration deadline = absl::Seconds(60);
  size_t max_messages = 1024;
};

class StdioExchange final : public ModernExchange {
 public:
  StdioExchange(std::unique_ptr<Transport> transport, StdioExchangeOptions options = {});

  absl::Status Start();
  absl::StatusOr<ModernExchangeResult> Execute(const Request& request) override;
  void Cancel() override;

 private:
  ModernExchangeResult FailedExchange(absl::Status status, ExecutionCertainty execution) const;

  std::unique_ptr<Transport> transport_;
  StdioExchangeOptions options_;
  absl::Mutex mutex_;
  bool started_ = false;
  bool cancelled_ = false;
};

}  // namespace slop::mcp::v2026_07_28

#endif  // SLOP_MCP_CLIENT_MODERN_STDIO_H_
