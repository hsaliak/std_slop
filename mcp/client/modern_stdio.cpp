#include "mcp/client/modern_stdio.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

#include "core/json_utils.h"
#include "mcp/client/modern.h"

namespace slop::mcp::v2026_07_28 {

StdioExchange::StdioExchange(std::unique_ptr<Transport> transport, StdioExchangeOptions options)
    : transport_(std::move(transport)), options_(options) {}

absl::Status StdioExchange::Start() {
  absl::MutexLock lock(mutex_);
  if (started_ || cancelled_) return absl::FailedPreconditionError("MCP stdio exchange can only be started once");
  if (transport_ == nullptr) return absl::InvalidArgumentError("MCP stdio exchange transport must not be null");
  if (options_.deadline <= absl::ZeroDuration() || options_.max_messages == 0 || options_.max_messages > 1024) {
    return absl::InvalidArgumentError("MCP stdio exchange limits must be positive and bounded");
  }
  const absl::Status status = transport_->Start();
  if (status.ok()) started_ = true;
  return status;
}

absl::StatusOr<ModernExchangeResult> StdioExchange::Execute(const Request& request, absl::Duration timeout) {
  absl::MutexLock lock(mutex_);
  if (!started_ || cancelled_) return absl::FailedPreconditionError("MCP stdio exchange is not running");
  if (timeout <= absl::ZeroDuration()) return absl::DeadlineExceededError("MCP request deadline expired");
  const absl::Duration budget = std::min(options_.deadline, timeout);
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  auto encoded_or = EncodeRequest(request);
  if (!encoded_or.ok()) return encoded_or.status();

  const absl::Duration send_timeout = budget - absl::FromChrono(std::chrono::steady_clock::now() - start);
  if (send_timeout <= absl::ZeroDuration()) return absl::DeadlineExceededError("MCP request deadline expired");
  const absl::Status send_status = transport_->Send(encoded_or->body, send_timeout);
  if (!send_status.ok()) {
    if (send_status.code() == absl::StatusCode::kUnavailable ||
        send_status.code() == absl::StatusCode::kDeadlineExceeded ||
        send_status.code() == absl::StatusCode::kInternal) {
      return FailedExchange(send_status, ExecutionCertainty::kMayHaveExecuted);
    }
    return send_status;
  }

  std::vector<nlohmann::json> messages;
  messages.reserve(options_.max_messages);
  for (size_t i = 0; i < options_.max_messages; ++i) {
    const absl::Duration elapsed = absl::FromChrono(std::chrono::steady_clock::now() - start);
    const absl::Duration remaining = budget - elapsed;
    if (remaining <= absl::ZeroDuration()) {
      (void)transport_->Close();
      return FailedExchange(absl::DeadlineExceededError("Timed out waiting for MCP stdio response"),
                            ExecutionCertainty::kMayHaveExecuted);
    }
    auto message_or = transport_->Receive(remaining);
    if (!message_or.ok()) return FailedExchange(message_or.status(), ExecutionCertainty::kMayHaveExecuted);
    const bool has_method = json_at(*message_or, "method") != nullptr;
    if (has_method && json_at(*message_or, "id") != nullptr) {
      (void)transport_->Close();
      return absl::InvalidArgumentError("MCP stdio server requests are not supported by this client");
    }
    messages.push_back(std::move(*message_or));
    if (!has_method) {
      auto result_or = NormalizeModernMessages(request, std::move(messages), options_.max_messages);
      if (!result_or.ok()) (void)transport_->Close();
      return result_or;
    }
  }
  (void)transport_->Close();
  return FailedExchange(absl::ResourceExhaustedError("MCP stdio message limit exceeded"),
                        ExecutionCertainty::kMayHaveExecuted);
}

void StdioExchange::Cancel() {
  absl::MutexLock lock(mutex_);
  cancelled_ = true;
  if (transport_ != nullptr) (void)transport_->Close();
}

ModernExchangeResult StdioExchange::FailedExchange(absl::Status status, ExecutionCertainty execution) const {
  ModernExchangeResult result;
  ProtocolFailure failure;
  failure.message = std::string(status.message());
  failure.execution = execution;
  result.failure = std::move(failure);
  return result;
}

}  // namespace slop::mcp::v2026_07_28
