#include "mcp/gateway/worker.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace slop::mcp::gateway {
namespace {

constexpr int kWorkerIpcFd = 3;
constexpr std::size_t kMaxIpcFrameBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxCallBytes = 1024 * 1024;
constexpr std::size_t kMaxWorkerOutstanding = 64;
constexpr auto kWorkerShutdownGrace = std::chrono::milliseconds(100);

using Clock = std::chrono::steady_clock;

int PollTimeoutMilliseconds(Clock::time_point deadline) {
  const auto remaining = deadline - Clock::now();
  if (remaining <= Clock::duration::zero()) return 0;
  auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  if (timeout == 0) timeout = 1;
  return static_cast<int>(std::min<std::int64_t>(timeout, std::numeric_limits<int>::max()));
}

absl::Status WaitForFd(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    pollfd descriptor{fd, events, 0};
    const int result = poll(&descriptor, 1, PollTimeoutMilliseconds(deadline));
    if (result > 0) {
      if ((descriptor.revents & POLLNVAL) != 0)
        return absl::FailedPreconditionError("worker IPC descriptor is invalid");
      if ((descriptor.revents & (events | POLLHUP | POLLERR)) != 0) return absl::OkStatus();
      continue;
    }
    if (result == 0) return absl::DeadlineExceededError("worker IPC deadline expired");
    if (errno != EINTR) return absl::UnavailableError(absl::StrCat("worker IPC poll failed: ", std::strerror(errno)));
  }
}

absl::Status TransferExact(int fd, void* bytes, std::size_t length, bool write, Clock::time_point deadline) {
  auto* cursor = static_cast<unsigned char*>(bytes);
  std::size_t transferred = 0;
  while (transferred < length) {
    const absl::Status ready = WaitForFd(fd, write ? POLLOUT : POLLIN, deadline);
    if (!ready.ok()) return ready;
    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags = MSG_NOSIGNAL;
#endif
    const ssize_t count = write ? send(fd, cursor + transferred, length - transferred, send_flags)
                                : recv(fd, cursor + transferred, length - transferred, 0);
    if (count > 0) {
      transferred += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) return absl::UnavailableError("worker IPC peer closed the channel");
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return absl::UnavailableError(absl::StrCat("worker IPC transfer failed: ", std::strerror(errno)));
  }
  return absl::OkStatus();
}

class FrameChannel {
 public:
  explicit FrameChannel(int fd) : fd_(fd) {}

  absl::Status Send(const nlohmann::json& message, Clock::time_point deadline) {
    std::string body = json_dump(message);
    if (body.empty() || body.size() > kMaxIpcFrameBytes) {
      return absl::ResourceExhaustedError("worker IPC frame exceeds size limit");
    }
    const std::uint32_t size = static_cast<std::uint32_t>(body.size());
    std::array<unsigned char, 4> header = {
        static_cast<unsigned char>((size >> 24) & 0xff), static_cast<unsigned char>((size >> 16) & 0xff),
        static_cast<unsigned char>((size >> 8) & 0xff), static_cast<unsigned char>(size & 0xff)};
    absl::Status status = TransferExact(fd_, header.data(), header.size(), true, deadline);
    if (!status.ok()) return status;
    return TransferExact(fd_, &body[0], body.size(), true, deadline);
  }

  absl::StatusOr<nlohmann::json> Receive(Clock::time_point deadline) {
    std::array<unsigned char, 4> header{};
    absl::Status status = TransferExact(fd_, header.data(), header.size(), false, deadline);
    if (!status.ok()) return status;
    const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24) |
                               (static_cast<std::uint32_t>(header[1]) << 16) |
                               (static_cast<std::uint32_t>(header[2]) << 8) | static_cast<std::uint32_t>(header[3]);
    if (size == 0 || size > kMaxIpcFrameBytes)
      return absl::ResourceExhaustedError("worker IPC frame length is invalid");
    std::string body(size, '\0');
    status = TransferExact(fd_, body.data(), body.size(), false, deadline);
    if (!status.ok()) return status;
    auto parsed = json_parse(body);
    if (!parsed.has_value() || !parsed->is_object())
      return absl::InvalidArgumentError("worker IPC frame is not a JSON object");
    return std::move(*parsed);
  }

 private:
  int fd_;
};

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

absl::Status CloseInheritedDescriptors(posix_spawn_file_actions_t* actions) {
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
  const int status = posix_spawn_file_actions_addclosefrom_np(actions, kWorkerIpcFd + 1);
  if (status != 0)
    return absl::UnavailableError(
        absl::StrCat("cannot configure worker descriptor isolation: ", std::strerror(status)));
  return absl::OkStatus();
#endif
#endif
  struct rlimit limit {};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return absl::UnavailableError("cannot inspect descriptor limit");
  const rlim_t upper = std::min<rlim_t>(limit.rlim_cur, 65536);
  for (int fd = kWorkerIpcFd + 1; static_cast<rlim_t>(fd) < upper; ++fd) {
    if (fcntl(fd, F_GETFD) >= 0) {
      const int status = posix_spawn_file_actions_addclose(actions, fd);
      if (status != 0) return absl::UnavailableError("cannot configure worker descriptor isolation");
    }
  }
  return absl::OkStatus();
}

class WorkerProcess {
 public:
  WorkerProcess(pid_t pid, int fd) : pid_(pid), fd_(fd) {}
  WorkerProcess(const WorkerProcess&) = delete;
  WorkerProcess& operator=(const WorkerProcess&) = delete;
  WorkerProcess(WorkerProcess&& other) noexcept : pid_(other.pid_), fd_(other.fd_) {
    other.pid_ = -1;
    other.fd_ = -1;
  }
  WorkerProcess& operator=(WorkerProcess&& other) noexcept {
    if (this != &other) {
      Stop();
      pid_ = other.pid_;
      fd_ = other.fd_;
      other.pid_ = -1;
      other.fd_ = -1;
    }
    return *this;
  }
  ~WorkerProcess() { Stop(); }

  int fd() const { return fd_; }
  void CloseChannel() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
  }

  void Stop() {
    CloseChannel();
    if (pid_ < 0) return;
    int status = 0;
    pid_t result;
    do {
      result = waitpid(pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == pid_ || (result < 0 && errno == ECHILD)) {
      pid_ = -1;
      return;
    }
    (void)kill(pid_, SIGTERM);
    const auto deadline = Clock::now() + kWorkerShutdownGrace;
    while (Clock::now() < deadline) {
      do {
        result = waitpid(pid_, &status, WNOHANG);
      } while (result < 0 && errno == EINTR);
      if (result == pid_ || (result < 0 && errno == ECHILD)) {
        pid_ = -1;
        return;
      }
      (void)poll(nullptr, 0, 10);
    }
    (void)kill(pid_, SIGKILL);
    do {
      result = waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);
    pid_ = -1;
  }

 private:
  pid_t pid_ = -1;
  int fd_ = -1;
};

absl::StatusOr<WorkerProcess> SpawnWorker(const std::string& executable_path) {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return absl::UnavailableError(absl::StrCat("cannot create worker IPC socket: ", std::strerror(errno)));
  }
  absl::Cleanup close_sockets = [&sockets] {
    if (sockets[0] >= 0) close(sockets[0]);
    if (sockets[1] >= 0) close(sockets[1]);
  };
  if (!SetNonBlocking(sockets[0]) || !SetNonBlocking(sockets[1])) {
    return absl::UnavailableError("cannot configure worker IPC socket");
  }
#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  (void)setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
  (void)setsockopt(sockets[1], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

  posix_spawn_file_actions_t actions;
  int spawn_status = posix_spawn_file_actions_init(&actions);
  if (spawn_status != 0) return absl::UnavailableError("cannot initialize worker spawn actions");
  absl::Cleanup destroy_actions = [&actions] { (void)posix_spawn_file_actions_destroy(&actions); };
  spawn_status = posix_spawn_file_actions_adddup2(&actions, sockets[1], kWorkerIpcFd);
  if (spawn_status != 0) return absl::UnavailableError("cannot configure worker IPC descriptor");
  spawn_status = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (spawn_status != 0) return absl::UnavailableError("cannot isolate worker stdin");
  spawn_status = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  if (spawn_status != 0) return absl::UnavailableError("cannot isolate worker stdout");
  spawn_status = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  if (spawn_status != 0) return absl::UnavailableError("cannot isolate worker stderr");
  absl::Status close_status = CloseInheritedDescriptors(&actions);
  if (!close_status.ok()) return close_status;

  std::string descriptor = std::to_string(kWorkerIpcFd);
  char* worker_argv[] = {const_cast<char*>(executable_path.c_str()), const_cast<char*>("--worker-fd"),
                         const_cast<char*>(descriptor.c_str()), nullptr};
  char* worker_env[] = {nullptr};
  pid_t pid = -1;
  spawn_status = posix_spawn(&pid, executable_path.c_str(), &actions, nullptr, worker_argv, worker_env);
  if (spawn_status != 0)
    return absl::UnavailableError(absl::StrCat("cannot start QuickJS worker: ", std::strerror(spawn_status)));
  close(sockets[1]);
  sockets[1] = -1;
  const int parent_fd = sockets[0];
  sockets[0] = -1;
  return WorkerProcess(pid, parent_fd);
}

std::uint64_t NextWorkerRunId() {
  static std::atomic<std::uint64_t> next_id{1};
  return next_id.fetch_add(1, std::memory_order_relaxed);
}

std::string StatusCodeName(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kInvalidArgument:
      return "invalid_argument";
    case absl::StatusCode::kDeadlineExceeded:
      return "deadline_exceeded";
    case absl::StatusCode::kResourceExhausted:
      return "resource_exhausted";
    case absl::StatusCode::kFailedPrecondition:
      return "failed_precondition";
    default:
      return "internal";
  }
}

absl::Status StatusFromName(const std::string& code, const std::string& message) {
  if (code == "invalid_argument") return absl::InvalidArgumentError(message);
  if (code == "deadline_exceeded") return absl::DeadlineExceededError(message);
  if (code == "resource_exhausted") return absl::ResourceExhaustedError(message);
  if (code == "failed_precondition") return absl::FailedPreconditionError(message);
  return absl::InternalError(message);
}

absl::Status CheckMessageFields(const nlohmann::json& message, const std::vector<std::string>& allowed) {
  if (!message.is_object()) return absl::InvalidArgumentError("worker IPC message must be an object");
  for (auto it = message.begin(); it != message.end(); ++it) {
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) {
      return absl::InvalidArgumentError("worker IPC message has an unknown field");
    }
  }
  return absl::OkStatus();
}

class IpcBroker final : public js_runtime::AsyncToolBroker {
 public:
  IpcBroker(int fd, std::uint64_t external_run_id, Clock::time_point deadline, nlohmann::json catalog)
      : channel_(fd), external_run_id_(external_run_id), deadline_(deadline), catalog_(std::move(catalog)) {}

  absl::Status Submit(const js_runtime::ToolRequest& request) override {
    if (broken_) return absl::UnavailableError("worker IPC channel is closed");
    if (request.deadline <= Clock::now()) return absl::DeadlineExceededError("tool deadline expired");
    if (runtime_run_id_ == 0) runtime_run_id_ = request.run_id;
    if (request.run_id != runtime_run_id_) return absl::InvalidArgumentError("worker runtime run ID changed");
    if (pending_.size() >= kMaxWorkerOutstanding || pending_.find(request.id) != pending_.end()) {
      return absl::ResourceExhaustedError("worker IPC call limit exceeded");
    }
    nlohmann::json message = {{"type", "call_request"}, {"runId", external_run_id_},
                              {"callId", request.id},   {"server", request.server},
                              {"tool", request.tool},   {"arguments", request.arguments}};
    const absl::Status status = channel_.Send(message, std::min(deadline_, request.deadline));
    if (!status.ok()) return status;
    pending_.emplace(request.id, true);
    return absl::OkStatus();
  }

  std::vector<js_runtime::ToolCompletion> TakeCompletions() override {
    std::vector<js_runtime::ToolCompletion> result;
    result.swap(completions_);
    return result;
  }

  void WaitForCompletion(std::chrono::milliseconds duration) override {
    if (broken_ || duration <= std::chrono::milliseconds::zero()) return;
    auto message = channel_.Receive(deadline_);
    if (!message.ok()) {
      if (message.status().code() == absl::StatusCode::kDeadlineExceeded) return;
      BreakChannel(std::string(message.status().message()));
      return;
    }
    const absl::Status status = ConsumeResult(*message);
    if (!status.ok()) BreakChannel(std::string(status.message()));
  }

  void CancelPending() override {
    if (broken_) return;
    (void)channel_.Send({{"type", "cancel"}, {"runId", external_run_id_}}, deadline_);
  }

  nlohmann::json PublicCatalog() const override { return catalog_; }
  absl::StatusOr<nlohmann::json> Help(const std::string& alias, const std::string& tool) const override {
    if (!catalog_.is_array()) return absl::InternalError("worker catalog is invalid");
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& server : catalog_) {
      const std::string server_alias = json_get_or(server, "alias", std::string());
      if (!alias.empty() && alias != server_alias) continue;
      const nlohmann::json* server_tools = json_at(server, "tools");
      if (server_tools == nullptr || !server_tools->is_array()) continue;
      for (const auto& item : *server_tools) {
        const std::string name = json_get_or(item, "name", std::string());
        if (!tool.empty() && tool != name) continue;
        nlohmann::json summary = item;
        summary.erase("inputSchema");
        summary["server"] = server_alias;
        if (!tool.empty()) {
          const nlohmann::json* schema = json_at(item, "inputSchema");
          if (schema != nullptr) summary["inputSchema"] = *schema;
          summary["result"] = {
              {"content", "array"}, {"structuredContent", "optional JSON value"}, {"isError", "boolean"}};
        }
        tools.push_back(std::move(summary));
      }
    }
    if ((!alias.empty() || !tool.empty()) && tools.empty())
      return absl::NotFoundError("tool or server is not in the authorized catalog");
    return nlohmann::json{{"servers", nlohmann::json::array()}, {"tools", std::move(tools)}};
  }

 private:
  absl::Status ConsumeResult(const nlohmann::json& message) {
    absl::Status fields = CheckMessageFields(message, {"type", "runId", "callId", "ok", "result", "error", "category"});
    if (!fields.ok()) return fields;
    if (json_get_or(message, "type", std::string()) != "call_result" ||
        json_get_or(message, "runId", std::uint64_t{0}) != external_run_id_) {
      return absl::InvalidArgumentError("worker IPC completion ID does not match");
    }
    const std::uint64_t id = json_get_or(message, "callId", std::uint64_t{0});
    auto pending = pending_.find(id);
    const auto ok = json_get<bool>(message, "ok");
    if (pending == pending_.end() || !ok.has_value())
      return absl::InvalidArgumentError("worker IPC completion is invalid");
    js_runtime::ToolCompletion completion{runtime_run_id_, id, *ok, nullptr, "", "tool"};
    if (*ok) {
      const nlohmann::json* result = json_at(message, "result");
      if (result == nullptr) return absl::InvalidArgumentError("worker IPC success has no result");
      completion.result = *result;
    } else {
      completion.error = json_get_or(message, "error", std::string("downstream tool failed"));
      completion.error_category = json_get_or(message, "category", std::string("downstream"));
    }
    pending_.erase(pending);
    completions_.push_back(std::move(completion));
    return absl::OkStatus();
  }

  void BreakChannel(std::string message) {
    broken_ = true;
    for (const auto& [id, unused] : pending_) {
      (void)unused;
      completions_.push_back({runtime_run_id_, id, false, nullptr, message, "ipc"});
    }
    pending_.clear();
  }

  FrameChannel channel_;
  std::uint64_t external_run_id_;
  Clock::time_point deadline_;
  nlohmann::json catalog_;
  std::uint64_t runtime_run_id_ = 0;
  std::map<std::uint64_t, bool> pending_;
  std::vector<js_runtime::ToolCompletion> completions_;
  bool broken_ = false;
};

absl::Status SendCallCompletion(FrameChannel* channel, std::uint64_t run_id,
                                const js_runtime::ToolCompletion& completion, Clock::time_point deadline) {
  nlohmann::json message = {
      {"type", "call_result"}, {"runId", run_id}, {"callId", completion.id}, {"ok", completion.ok}};
  if (completion.ok) {
    message["result"] = completion.result;
  } else {
    message["error"] = completion.error;
    message["category"] = completion.error_category;
  }
  return channel->Send(message, deadline);
}

}  // namespace

absl::StatusOr<WorkerCallRequest> ParseWorkerCallRequest(const nlohmann::json& message, std::uint64_t expected_run_id) {
  absl::Status fields = CheckMessageFields(message, {"type", "runId", "callId", "server", "tool", "arguments"});
  if (!fields.ok()) return fields;
  const auto type = json_get<std::string>(message, "type");
  const auto run_id = json_get<std::uint64_t>(message, "runId");
  const auto call_id = json_get<std::uint64_t>(message, "callId");
  const auto server = json_get<std::string>(message, "server");
  const auto tool = json_get<std::string>(message, "tool");
  const nlohmann::json* arguments = json_at(message, "arguments");
  if (!type || *type != "call_request" || !run_id || *run_id != expected_run_id || !call_id || *call_id == 0 ||
      !server || server->empty() || server->size() > 64 || !tool || tool->empty() || tool->size() > 256 ||
      arguments == nullptr || !arguments->is_object()) {
    return absl::InvalidArgumentError("worker IPC call request is invalid");
  }
  if (json_dump(message).size() > kMaxCallBytes)
    return absl::ResourceExhaustedError("worker IPC call exceeds size limit");
  return WorkerCallRequest{*run_id, *call_id, *server, *tool, *arguments};
}

absl::StatusOr<nlohmann::json> ExecuteInWorker(const std::string& executable_path, const std::string& code,
                                               const nlohmann::json& input, js_runtime::AsyncToolBroker* broker,
                                               js_runtime::RuntimeOptions options, std::uint64_t trace_id) {
  if (broker == nullptr) return absl::InvalidArgumentError("tool broker must not be null");
  if (executable_path.empty() || executable_path[0] != '/')
    return absl::InvalidArgumentError("worker executable path must be absolute");
  if (options.timeout <= std::chrono::milliseconds::zero())
    return absl::InvalidArgumentError("worker timeout must be positive");
  static std::atomic_flag worker_active = ATOMIC_FLAG_INIT;
  if (worker_active.test_and_set(std::memory_order_acquire)) {
    return absl::FailedPreconditionError("only one run_js worker may be active at a time");
  }
  absl::Cleanup release_worker = [] { worker_active.clear(std::memory_order_release); };
  auto worker_or = SpawnWorker(executable_path);
  if (!worker_or.ok()) return worker_or.status();
  WorkerProcess worker = std::move(*worker_or);
  FrameChannel channel(worker.fd());
  const std::uint64_t run_id = trace_id == 0 ? NextWorkerRunId() : trace_id;
  const Clock::time_point deadline = Clock::now() + options.timeout;
  nlohmann::json start = {{"type", "start_run"},
                          {"runId", run_id},
                          {"code", code},
                          {"input", input},
                          {"catalog", broker->PublicCatalog()},
                          {"timeoutMs", options.timeout.count()},
                          {"memoryLimit", options.memory_limit_bytes},
                          {"maxCodeBytes", options.max_code_bytes},
                          {"maxOutputBytes", options.max_output_bytes},
                          {"maxPendingCalls", options.max_pending_calls},
                          {"maxJobsPerTurn", options.max_jobs_per_turn}};
  absl::Status status = channel.Send(start, deadline);
  if (!status.ok()) return status;

  std::map<std::uint64_t, bool> submitted;
  absl::Cleanup cancel_unresolved = [&broker, &submitted] {
    if (!submitted.empty()) broker->CancelPending();
  };
  while (Clock::now() < deadline) {
    for (const js_runtime::ToolCompletion& completion : broker->TakeCompletions()) {
      if (completion.run_id != run_id) continue;
      auto found = submitted.find(completion.id);
      if (found == submitted.end()) continue;
      status = SendCallCompletion(&channel, run_id, completion, deadline);
      submitted.erase(found);
      if (!status.ok()) return status;
    }
    pollfd descriptor{worker.fd(), POLLIN, 0};
    const int poll_result = poll(&descriptor, 1, 10);
    if (poll_result < 0 && errno == EINTR) continue;
    if (poll_result < 0) return absl::UnavailableError("worker IPC polling failed");
    if (poll_result == 0) continue;
    auto message_or = channel.Receive(deadline);
    if (!message_or.ok()) return message_or.status();
    const std::string type = json_get_or(*message_or, "type", std::string());
    if (type == "call_request") {
      auto request_or = ParseWorkerCallRequest(*message_or, run_id);
      if (!request_or.ok()) return request_or.status();
      if (submitted.size() >= kMaxWorkerOutstanding || submitted.find(request_or->call_id) != submitted.end()) {
        return absl::ResourceExhaustedError("worker outstanding-call limit exceeded");
      }
      submitted.emplace(request_or->call_id, true);
      const js_runtime::ToolRequest request{run_id,           request_or->call_id,  deadline, request_or->server,
                                            request_or->tool, request_or->arguments};
      status = broker->Submit(request);
      if (!status.ok()) {
        js_runtime::ToolCompletion rejected{run_id,  request_or->call_id,           false,
                                            nullptr, std::string(status.message()), "admission"};
        status = SendCallCompletion(&channel, run_id, rejected, deadline);
        submitted.erase(request_or->call_id);
        if (!status.ok()) return status;
      }
      continue;
    }
    if (type == "cancel") {
      const absl::Status fields = CheckMessageFields(*message_or, {"type", "runId"});
      if (!fields.ok() || json_get_or(*message_or, "runId", std::uint64_t{0}) != run_id) {
        return absl::InvalidArgumentError("worker cancel message is invalid");
      }
      broker->CancelPending();
      continue;
    }
    if (type == "final_result") {
      const absl::Status fields = CheckMessageFields(*message_or, {"type", "runId", "result"});
      if (!fields.ok() || json_get_or(*message_or, "runId", std::uint64_t{0}) != run_id) {
        return absl::InvalidArgumentError("worker final result is invalid");
      }
      const nlohmann::json* result = json_at(*message_or, "result");
      if (result == nullptr || json_dump(*result).size() > options.max_output_bytes) {
        return absl::ResourceExhaustedError("worker result is missing or over size limit");
      }
      return *result;
    }
    if (type == "final_error") {
      const absl::Status fields = CheckMessageFields(*message_or, {"type", "runId", "code", "message"});
      const auto code = json_get<std::string>(*message_or, "code");
      const auto error = json_get<std::string>(*message_or, "message");
      if (!fields.ok() || json_get_or(*message_or, "runId", std::uint64_t{0}) != run_id || !code || !error) {
        return absl::InvalidArgumentError("worker final error is invalid");
      }
      return StatusFromName(*code, *error);
    }
    return absl::InvalidArgumentError("worker sent an unknown IPC message");
  }
  return absl::DeadlineExceededError("JavaScript worker exceeded its time limit");
}

int RunWorkerMode(int ipc_fd) {
  signal(SIGPIPE, SIG_IGN);
  FrameChannel channel(ipc_fd);
  const Clock::time_point startup_deadline = Clock::now() + std::chrono::seconds(15);
  auto start_or = channel.Receive(startup_deadline);
  if (!start_or.ok()) return 2;
  const absl::Status fields =
      CheckMessageFields(*start_or, {"type", "runId", "code", "input", "catalog", "timeoutMs", "memoryLimit",
                                     "maxCodeBytes", "maxOutputBytes", "maxPendingCalls", "maxJobsPerTurn"});
  const auto run_id = json_get<std::uint64_t>(*start_or, "runId");
  const auto code = json_get<std::string>(*start_or, "code");
  const nlohmann::json* input = json_at(*start_or, "input");
  const nlohmann::json* catalog = json_at(*start_or, "catalog");
  if (!fields.ok() || json_get_or(*start_or, "type", std::string()) != "start_run" || !run_id || *run_id == 0 ||
      !code || input == nullptr || !input->is_object() || catalog == nullptr || !catalog->is_array()) {
    return 2;
  }
  js_runtime::RuntimeOptions options;
  const auto timeout_ms = json_get<std::int64_t>(*start_or, "timeoutMs");
  const auto memory = json_get<std::size_t>(*start_or, "memoryLimit");
  const auto max_code = json_get<std::size_t>(*start_or, "maxCodeBytes");
  const auto max_output = json_get<std::size_t>(*start_or, "maxOutputBytes");
  const auto max_calls = json_get<std::size_t>(*start_or, "maxPendingCalls");
  const auto max_jobs = json_get<std::size_t>(*start_or, "maxJobsPerTurn");
  if (!timeout_ms || *timeout_ms <= 0 || *timeout_ms > 60'000 || !memory || *memory > 128 * 1024 * 1024 || !max_code ||
      *max_code > 1024 * 1024 || !max_output || *max_output > kMaxIpcFrameBytes || !max_calls ||
      *max_calls > kMaxWorkerOutstanding || !max_jobs || *max_jobs > 4096) {
    return 2;
  }
  options.timeout = std::chrono::milliseconds(*timeout_ms);
  options.memory_limit_bytes = *memory;
  options.max_code_bytes = *max_code;
  options.max_output_bytes = *max_output;
  options.max_pending_calls = *max_calls;
  options.max_jobs_per_turn = *max_jobs;
  const Clock::time_point deadline = Clock::now() + options.timeout;
  IpcBroker broker(ipc_fd, *run_id, deadline, *catalog);
  auto result = js_runtime::Run(*code, *input, &broker, options);
  nlohmann::json final;
  if (result.ok()) {
    final = {{"type", "final_result"}, {"runId", *run_id}, {"result", std::move(*result)}};
  } else {
    final = {{"type", "final_error"},
             {"runId", *run_id},
             {"code", StatusCodeName(result.status().code())},
             {"message", std::string(result.status().message()).substr(0, 4096)}};
  }
  return channel.Send(final, deadline).ok() ? 0 : 3;
}

}  // namespace slop::mcp::gateway
