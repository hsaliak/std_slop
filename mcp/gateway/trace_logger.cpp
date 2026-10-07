#include "mcp/gateway/trace_logger.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxLogPathBytes = 4096;
constexpr char kRecordSeparator[] = "================================================================================\n";

std::string ErrorText(const char* operation, int error_number) {
  return absl::StrCat(operation, ": ", std::strerror(error_number));
}

std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  std::tm utc_time{};
  if (gmtime_r(&now, &utc_time) == nullptr) return "unknown-time";
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) return "unknown-time";
  return buffer;
}

nlohmann::json RawResultJson(const ToolCallResult& result) {
  nlohmann::json raw = {{"content", result.content},
                        {"isError", result.is_error},
                        {"meta", result.meta},
                        {"kind", result.kind == ToolResultKind::kComplete ? "complete" : "input_required"}};
  if (result.structured_content.has_value()) raw["structuredContent"] = *result.structured_content;
  if (result.request_state.has_value()) raw["requestState"] = *result.request_state;
  return raw;
}

std::string StatusJson(const absl::Status& status) {
  return json_dump({{"code", static_cast<int>(status.code())}, {"message", std::string(status.message())}}, 2);
}

std::string FormatCodeForLog(const std::string& code) {
  std::string result;
  result.reserve(code.size());
  for (unsigned char character : code) {
    if (character == '\n' || character == '\t' || (character >= 0x20 && character != 0x7f)) {
      result.push_back(static_cast<char>(character));
      continue;
    }
    char escaped[5];
    std::snprintf(escaped, sizeof(escaped), "\\x%02x", character);
    result.append(escaped);
  }
  return result;
}

std::string ToolFields(const std::string& server, const std::string& tool) {
  return absl::StrCat("server: ", json_dump(nlohmann::json(server)), "\ntool: ", json_dump(nlohmann::json(tool)), "\n");
}

}  // namespace

absl::StatusOr<std::shared_ptr<TraceLogger>> TraceLogger::Open(const std::string& path) {
  if (path.empty() || path.size() > kMaxLogPathBytes || path.find('\0') != std::string::npos ||
      !std::filesystem::path(path).is_absolute()) {
    return absl::InvalidArgumentError("trace log path must be a bounded absolute path");
  }
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (fd < 0) return absl::UnavailableError(ErrorText("cannot open trace log", errno));

  struct stat file_info {};
  if (fstat(fd, &file_info) != 0) {
    const int error_number = errno;
    close(fd);
    return absl::UnavailableError(ErrorText("cannot inspect trace log", error_number));
  }
  if (!S_ISREG(file_info.st_mode)) {
    close(fd);
    return absl::InvalidArgumentError("trace log path must name a regular file");
  }
  if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    const int error_number = errno;
    close(fd);
    return absl::UnavailableError(ErrorText("cannot restrict trace log permissions", error_number));
  }
  return std::make_shared<TraceLogger>(fd);
}

TraceLogger::~TraceLogger() {
  if (fd_ >= 0) close(fd_);
}

std::uint64_t TraceLogger::BeginRun(const std::string& code, const nlohmann::json& input) {
  const std::uint64_t trace_id = next_trace_id_.fetch_add(1, std::memory_order_relaxed);
  std::string record = EventHeader("RUN START", trace_id);
  record.append("input:\n").append(json_dump(input, 2)).append("\ncode (terminal-safe; control bytes escaped):\n");
  record.append(FormatCodeForLog(code));
  (void)AppendRecord(std::move(record));
  return trace_id;
}

void TraceLogger::LogToolCall(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                              const std::string& tool, const nlohmann::json& arguments) {
  std::string record = EventHeader(absl::StrCat("TOOL CALL ", call_id), trace_id);
  record.append(ToolFields(server, tool)).append("arguments:\n");
  record.append(json_dump(arguments, 2));
  (void)AppendRecord(std::move(record));
}

void TraceLogger::LogToolResult(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                                const std::string& tool, const ToolCallResult& raw_result,
                                const absl::StatusOr<NormalizedToolResult>& normalized_result) {
  std::string record = EventHeader(absl::StrCat("TOOL RESULT ", call_id), trace_id);
  record.append(ToolFields(server, tool)).append("raw downstream result:\n");
  record.append(json_dump(RawResultJson(raw_result), 2)).append("\nJavaScript-visible result:\n");
  if (normalized_result.ok()) {
    record.append(json_dump({{"ok", normalized_result->ok},
                             {"value", normalized_result->value},
                             {"errorCategory", normalized_result->error_category},
                             {"error", normalized_result->error}},
                            2));
  } else {
    record.append(StatusJson(normalized_result.status()));
  }
  (void)AppendRecord(std::move(record));
}

void TraceLogger::LogToolFailure(std::uint64_t trace_id, std::uint64_t call_id, const std::string& server,
                                 const std::string& tool, const absl::Status& status) {
  std::string record = EventHeader(absl::StrCat("TOOL FAILURE ", call_id), trace_id);
  record.append(ToolFields(server, tool)).append("error:\n");
  record.append(StatusJson(status));
  (void)AppendRecord(std::move(record));
}

void TraceLogger::FinishRun(std::uint64_t trace_id, const nlohmann::json& result) {
  std::string record = EventHeader("RUN SUCCESS", trace_id);
  record.append("result:\n").append(json_dump(result, 2));
  (void)AppendRecord(std::move(record));
}

void TraceLogger::FailRun(std::uint64_t trace_id, const absl::Status& status) {
  std::string record = EventHeader("RUN FAILURE", trace_id);
  record.append("error:\n").append(StatusJson(status));
  (void)AppendRecord(std::move(record));
}

absl::Status TraceLogger::AppendRecord(std::string record) {
  if (record.empty() || record.back() != '\n') record.push_back('\n');
  record.append(kRecordSeparator);
  absl::MutexLock lock(mutex_);
  std::size_t written = 0;
  while (written < record.size()) {
    const ssize_t count = write(fd_, record.data() + written, record.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      const int error_number = count == 0 ? EIO : errno;
      const absl::Status status = absl::UnavailableError(ErrorText("cannot write trace log", error_number));
      std::cerr << "run_js_server: " << status.message() << '\n';
      return status;
    }
    written += static_cast<std::size_t>(count);
  }
  return absl::OkStatus();
}

std::string TraceLogger::EventHeader(const std::string& event, std::uint64_t trace_id) const {
  return absl::StrCat("\n", kRecordSeparator, TimestampUtc(),
                      " trace=", trace_id, " event=", event, "\n");
}

}  // namespace slop::mcp::gateway
