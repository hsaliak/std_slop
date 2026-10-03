#include "mcp/client/stdio_transport.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

#include "core/json_utils.h"
#include "mcp/client/stdio_transport_internal.h"
#include "mcp/json_rpc.h"

#include <sys/types.h>
#include <sys/wait.h>

extern char** environ;

namespace slop::mcp {
namespace {

constexpr size_t kReadBufferBytes = 4096;
constexpr size_t kMaxArgumentCount = 256;
constexpr size_t kMaxArgumentBytes = 1024 * 1024;
constexpr int kTerminateGraceMs = 100;
constexpr int kWaitSliceMs = 10;

absl::Status SetCloseOnExec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    return absl::InternalError(absl::StrCat("Failed to set close-on-exec on pipe: ", std::strerror(errno)));
  }
  return absl::OkStatus();
}

absl::Status SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return absl::InternalError(absl::StrCat("Failed to set nonblocking MCP pipe: ", std::strerror(errno)));
  }
  return absl::OkStatus();
}

int PollTimeoutMs(int64_t timeout_ms, std::chrono::steady_clock::time_point start) {
  const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  if (elapsed_ms >= timeout_ms) return 0;
  return static_cast<int>(std::min<int64_t>(timeout_ms - elapsed_ms, INT_MAX));
}

absl::Status WaitForWritable(int fd, int64_t timeout_ms, std::chrono::steady_clock::time_point start) {
  while (true) {
    pollfd descriptor{fd, POLLOUT, 0};
    const int result = poll(&descriptor, 1, PollTimeoutMs(timeout_ms, start));
    if (result == 0) return absl::DeadlineExceededError("Timed out writing to MCP stdio server");
    if (result < 0) {
      if (errno == EINTR) continue;
      return absl::UnavailableError(absl::StrCat("Failed polling MCP stdio input: ", std::strerror(errno)));
    }
    if (descriptor.revents & POLLNVAL) return absl::UnavailableError("MCP stdio input pipe is invalid");
    if (descriptor.revents & POLLOUT) return absl::OkStatus();
    if (descriptor.revents & (POLLERR | POLLHUP)) {
      return absl::UnavailableError("MCP stdio server closed its input");
    }
  }
}

void ConsumeGeneratedSigpipe(const sigset_t& sigpipe_set) {
  const timespec no_wait{0, 0};
  while (true) {
    const int result = sigtimedwait(&sigpipe_set, nullptr, &no_wait);
    if (result == SIGPIPE || (result == -1 && errno == EAGAIN)) return;
    if (result == -1 && errno == EINTR) continue;
    return;
  }
}

int64_t DurationToTimeoutMs(absl::Duration timeout) {
  if (timeout == absl::InfiniteDuration()) return INT64_MAX;
  return std::max<int64_t>(0, absl::ToInt64Milliseconds(timeout));
}

}  // namespace

namespace stdio_internal {

absl::StatusOr<std::optional<std::string>> PopFrame(std::string* buffer, size_t max_frame_bytes) {
  if (buffer == nullptr) return absl::InvalidArgumentError("MCP stdio frame buffer must not be null");
  const size_t newline = buffer->find('\n');
  if (newline == std::string::npos) {
    if (buffer->size() > max_frame_bytes) {
      return absl::ResourceExhaustedError("MCP stdio response frame exceeds the size limit");
    }
    return std::optional<std::string>();
  }
  if (newline > max_frame_bytes) {
    return absl::ResourceExhaustedError("MCP stdio response frame exceeds the size limit");
  }
  std::string frame = buffer->substr(0, newline);
  buffer->erase(0, newline + 1);
  return std::optional<std::string>(std::move(frame));
}

}  // namespace stdio_internal

StdioTransport::StdioTransport(StdioTransportOptions options) : options_(std::move(options)) {}

StdioTransport::~StdioTransport() { (void)Close(); }

absl::Status StdioTransport::Start() {
  if (started_ || closed_) return absl::FailedPreconditionError("MCP stdio transport can only be started once");
  if (options_.command.empty()) return absl::InvalidArgumentError("MCP stdio command must not be empty");
  if (options_.max_request_bytes == 0 || options_.max_response_bytes == 0) {
    return absl::InvalidArgumentError("MCP stdio frame limits must be greater than zero");
  }
  if (options_.args.size() + 1 > kMaxArgumentCount) {
    return absl::ResourceExhaustedError("MCP stdio command has too many arguments");
  }

  if (options_.command.size() >= kMaxArgumentBytes) {
    return absl::ResourceExhaustedError("MCP stdio command arguments exceed the size limit");
  }
  size_t argument_bytes = options_.command.size() + 1;
  if (options_.command.find('\0') != std::string::npos) {
    return absl::InvalidArgumentError("MCP stdio command contains a null byte");
  }
  for (const std::string& arg : options_.args) {
    if (arg.find('\0') != std::string::npos)
      return absl::InvalidArgumentError("MCP stdio argument contains a null byte");
    if (argument_bytes >= kMaxArgumentBytes || arg.size() >= kMaxArgumentBytes - argument_bytes) {
      return absl::ResourceExhaustedError("MCP stdio command arguments exceed the size limit");
    }
    argument_bytes += arg.size() + 1;
  }
  if (argument_bytes > kMaxArgumentBytes) {
    return absl::ResourceExhaustedError("MCP stdio command arguments exceed the size limit");
  }

  int child_input_pipe[2] = {-1, -1};
  int child_output_pipe[2] = {-1, -1};
  absl::Cleanup pipe_cleanup = [&] {
    for (int* fd : {&child_input_pipe[0], &child_input_pipe[1], &child_output_pipe[0], &child_output_pipe[1]}) {
      if (*fd == -1) continue;
      close(*fd);
      *fd = -1;
    }
  };
  if (pipe(child_input_pipe) != 0) {
    return absl::UnavailableError(absl::StrCat("Failed to create MCP stdio input pipe: ", std::strerror(errno)));
  }
  if (pipe(child_output_pipe) != 0) {
    return absl::UnavailableError(absl::StrCat("Failed to create MCP stdio output pipe: ", std::strerror(errno)));
  }
  for (const int fd : {child_input_pipe[0], child_input_pipe[1], child_output_pipe[0], child_output_pipe[1]}) {
    const absl::Status status = SetCloseOnExec(fd);
    if (!status.ok()) return status;
  }

  posix_spawn_file_actions_t actions;
  const int actions_status = posix_spawn_file_actions_init(&actions);
  if (actions_status != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to initialize MCP process actions: ", std::strerror(actions_status)));
  }
  absl::Cleanup actions_cleanup = [&] { (void)posix_spawn_file_actions_destroy(&actions); };
  int action_status = posix_spawn_file_actions_adddup2(&actions, child_input_pipe[0], STDIN_FILENO);
  if (action_status == 0) {
    action_status = posix_spawn_file_actions_adddup2(&actions, child_output_pipe[1], STDOUT_FILENO);
  }
  for (const int fd : {child_input_pipe[0], child_input_pipe[1], child_output_pipe[0], child_output_pipe[1]}) {
    if (action_status != 0) break;
    action_status = posix_spawn_file_actions_addclose(&actions, fd);
  }
  if (action_status != 0) {
    return absl::InternalError(absl::StrCat("Failed to configure MCP process pipes: ", std::strerror(action_status)));
  }

  std::vector<std::string> arguments;
  arguments.reserve(options_.args.size() + 1);
  arguments.push_back(options_.command);
  arguments.insert(arguments.end(), options_.args.begin(), options_.args.end());
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int spawn_status = posix_spawnp(&pid, options_.command.c_str(), &actions, nullptr, argv.data(), environ);
  if (spawn_status != 0) {
    return absl::UnavailableError(absl::StrCat("Failed to start MCP stdio command: ", std::strerror(spawn_status)));
  }

  child_pid_ = pid;
  input_fd_ = child_input_pipe[1];
  child_input_pipe[1] = -1;
  output_fd_ = child_output_pipe[0];
  child_output_pipe[0] = -1;
  started_ = true;
  close(child_input_pipe[0]);
  child_input_pipe[0] = -1;
  close(child_output_pipe[1]);
  child_output_pipe[1] = -1;

  absl::Status status = SetNonBlocking(input_fd_);
  if (status.ok()) status = SetNonBlocking(output_fd_);
  if (!status.ok()) return FailAndClose(status);
  return absl::OkStatus();
}

absl::Status StdioTransport::Send(const nlohmann::json& message) {
  if (!started_ || closed_) return absl::FailedPreconditionError("MCP stdio transport is not running");
  if (!message.is_object()) return absl::InvalidArgumentError("MCP outbound message must be an object");
  std::string frame = json_dump(message);
  auto parsed_or = ParseJsonRpcMessage(frame);
  if (!parsed_or.ok()) return parsed_or.status();
  if (frame.size() > options_.max_request_bytes) {
    return absl::ResourceExhaustedError("MCP stdio request frame exceeds the size limit");
  }
  frame.push_back('\n');
  const absl::Status status = WriteFrame(frame);
  if (!status.ok()) return FailAndClose(status);
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> StdioTransport::Receive(absl::Duration timeout) {
  if (!started_ || closed_) return absl::FailedPreconditionError("MCP stdio transport is not running");
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const int64_t timeout_ms = DurationToTimeoutMs(timeout);
  std::array<char, kReadBufferBytes> bytes{};

  while (true) {
    auto frame_or = stdio_internal::PopFrame(&buffered_output_, options_.max_response_bytes);
    if (!frame_or.ok()) return FailAndClose(frame_or.status());
    if (frame_or->has_value()) {
      auto message_or = ParseJsonRpcMessage(**frame_or);
      if (!message_or.ok()) return FailAndClose(message_or.status());
      return std::move(*message_or);
    }

    pollfd descriptor{output_fd_, POLLIN, 0};
    const int poll_result = poll(&descriptor, 1, PollTimeoutMs(timeout_ms, start));
    if (poll_result == 0) return FailAndClose(absl::DeadlineExceededError("Timed out waiting for MCP stdio response"));
    if (poll_result < 0) {
      if (errno == EINTR) continue;
      return FailAndClose(
          absl::UnavailableError(absl::StrCat("Failed polling MCP stdio output: ", std::strerror(errno))));
    }
    if (descriptor.revents & POLLNVAL) return FailAndClose(absl::UnavailableError("MCP stdio output pipe is invalid"));
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

    const size_t remaining = options_.max_response_bytes - buffered_output_.size();
    const size_t read_size = std::min(bytes.size(), remaining + 1);
    const ssize_t bytes_read = read(output_fd_, bytes.data(), read_size);
    if (bytes_read > 0) {
      buffered_output_.append(bytes.data(), static_cast<size_t>(bytes_read));
      continue;
    }
    if (bytes_read == 0) return FailAndClose(absl::UnavailableError("MCP stdio server closed its output"));
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return FailAndClose(
        absl::UnavailableError(absl::StrCat("Failed reading MCP stdio output: ", std::strerror(errno))));
  }
}

absl::Status StdioTransport::Close() {
  if (closed_) return ReapChild();
  closed_ = true;
  if (input_fd_ != -1) {
    close(input_fd_);
    input_fd_ = -1;
  }
  if (output_fd_ != -1) {
    close(output_fd_);
    output_fd_ = -1;
  }
  buffered_output_.clear();
  return ReapChild();
}

absl::Status StdioTransport::WriteFrame(const std::string& frame) {
  sigset_t sigpipe_set;
  sigset_t original_mask;
  sigset_t pending_signals;
  sigemptyset(&sigpipe_set);
  sigaddset(&sigpipe_set, SIGPIPE);
  const int mask_status = pthread_sigmask(SIG_BLOCK, &sigpipe_set, &original_mask);
  if (mask_status != 0) {
    return absl::InternalError(
        absl::StrCat("Failed to block SIGPIPE while writing MCP frame: ", std::strerror(mask_status)));
  }
  if (sigpending(&pending_signals) != 0) {
    (void)pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
    return absl::InternalError(absl::StrCat("Failed to inspect pending signals: ", std::strerror(errno)));
  }
  const bool sigpipe_was_pending = sigismember(&pending_signals, SIGPIPE) == 1;
  bool generated_sigpipe = false;
  absl::Status status;
  size_t offset = 0;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const int64_t timeout_ms = DurationToTimeoutMs(options_.send_timeout);

  while (offset < frame.size()) {
    status = WaitForWritable(input_fd_, timeout_ms, start);
    if (!status.ok()) break;
    const ssize_t bytes_written = write(input_fd_, frame.data() + offset, frame.size() - offset);
    if (bytes_written > 0) {
      offset += static_cast<size_t>(bytes_written);
      continue;
    }
    if (bytes_written < 0 && errno == EINTR) continue;
    if (bytes_written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (bytes_written < 0 && errno == EPIPE) generated_sigpipe = true;
    status = absl::UnavailableError(absl::StrCat("Failed writing MCP stdio input: ", std::strerror(errno)));
    break;
  }

  if (generated_sigpipe && !sigpipe_was_pending) ConsumeGeneratedSigpipe(sigpipe_set);
  const int restore_status = pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
  if (restore_status != 0 && status.ok()) {
    return absl::InternalError(
        absl::StrCat("Failed to restore signal mask after MCP write: ", std::strerror(restore_status)));
  }
  return status;
}

absl::Status StdioTransport::FailAndClose(absl::Status status) {
  const absl::Status close_status = Close();
  if (close_status.ok()) return status;
  return absl::Status(status.code(),
                      absl::StrCat(status.message(), "; MCP child cleanup failed: ", close_status.message()));
}

absl::Status StdioTransport::ReapChild() {
  if (child_pid_ < 0) return absl::OkStatus();
  const pid_t pid = static_cast<pid_t>(child_pid_);

  int child_status = 0;
  pid_t wait_result;
  do {
    wait_result = waitpid(pid, &child_status, WNOHANG);
  } while (wait_result < 0 && errno == EINTR);
  if (wait_result == pid || (wait_result < 0 && errno == ECHILD)) {
    child_pid_ = -1;
    return absl::OkStatus();
  }
  if (wait_result < 0) {
    return absl::UnavailableError(absl::StrCat("Failed to inspect MCP child process: ", std::strerror(errno)));
  }

  if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
    return absl::UnavailableError(absl::StrCat("Failed to stop MCP child process: ", std::strerror(errno)));
  }
  for (int waited_ms = 0; waited_ms < kTerminateGraceMs; waited_ms += kWaitSliceMs) {
    (void)poll(nullptr, 0, kWaitSliceMs);
    do {
      wait_result = waitpid(pid, &child_status, WNOHANG);
    } while (wait_result < 0 && errno == EINTR);
    if (wait_result == pid || (wait_result < 0 && errno == ECHILD)) {
      child_pid_ = -1;
      return absl::OkStatus();
    }
    if (wait_result < 0) {
      return absl::UnavailableError(absl::StrCat("Failed to wait for MCP child process: ", std::strerror(errno)));
    }
  }

  if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
    return absl::UnavailableError(absl::StrCat("Failed to kill MCP child process: ", std::strerror(errno)));
  }
  do {
    wait_result = waitpid(pid, &child_status, 0);
  } while (wait_result < 0 && errno == EINTR);
  if (wait_result < 0 && errno != ECHILD) {
    return absl::UnavailableError(absl::StrCat("Failed to reap MCP child process: ", std::strerror(errno)));
  }
  child_pid_ = -1;
  return absl::OkStatus();
}

}  // namespace slop::mcp
