#include "mcp/server/stdio.h"

#include <algorithm>
#include <istream>
#include <ostream>
#include <string>

#include "core/json_utils.h"

namespace slop::mcp::server {
namespace {

absl::Status Fail(std::ostream& diagnostics, absl::Status status) {
  diagnostics << "mcp stdio: " << status.message() << '\n';
  diagnostics.flush();
  return status;
}

}  // namespace

absl::Status RunStdio(const Server& server, std::istream& input, std::ostream& output, std::ostream& diagnostics,
                      StdioOptions options) {
  // Reject unsafe stream configurations before writing even a diagnostic.
  if (input.exceptions() != std::ios::goodbit || output.exceptions() != std::ios::goodbit ||
      diagnostics.exceptions() != std::ios::goodbit) {
    return absl::InvalidArgumentError("MCP streams must have exceptions disabled");
  }
  if (input.rdbuf() == output.rdbuf() || input.rdbuf() == diagnostics.rdbuf() ||
      output.rdbuf() == diagnostics.rdbuf()) {
    return absl::InvalidArgumentError("MCP streams must use separate buffers");
  }
  if (options.max_input_bytes == 0) {
    return Fail(diagnostics, absl::InvalidArgumentError("MCP input line limit must be nonzero"));
  }
  if (!output.good()) return Fail(diagnostics, absl::InternalError("Failed to write MCP output"));

  std::string line;
  line.reserve(std::min(options.max_input_bytes, size_t{4096}));
  while (true) {
    const auto byte = input.get();
    if (input.bad()) return Fail(diagnostics, absl::InternalError("Failed to read MCP input"));
    if (byte == std::char_traits<char>::eof()) {
      if (!input.eof()) return Fail(diagnostics, absl::InternalError("Failed to read MCP input"));
      if (!line.empty()) return Fail(diagnostics, absl::DataLossError("MCP input ended before newline"));
      return absl::OkStatus();
    }
    if (byte == '\n') {
      const auto reply = server.Dispatch(line);
      line.clear();
      if (reply) {
        output << json_dump(*reply) << '\n';
        if (!output.good()) return Fail(diagnostics, absl::InternalError("Failed to write MCP output"));
        output.flush();
        if (!output.good()) return Fail(diagnostics, absl::InternalError("Failed to flush MCP output"));
      }
      continue;
    }
    if (line.size() == options.max_input_bytes) {
      return Fail(diagnostics, absl::ResourceExhaustedError("MCP input line exceeds byte limit"));
    }
    line.push_back(static_cast<char>(byte));
  }
}

}  // namespace slop::mcp::server
