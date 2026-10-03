#ifndef SLOP_MCP_CLIENT_STDIO_TRANSPORT_INTERNAL_H_
#define SLOP_MCP_CLIENT_STDIO_TRANSPORT_INTERNAL_H_

#include <cstddef>
#include <optional>
#include <string>

#include "absl/status/statusor.h"

namespace slop::mcp::stdio_internal {

// Removes one complete LF-terminated frame. On no complete frame, leaves the
// buffer unchanged. The frame-size limit excludes LF and includes a CR byte.
absl::StatusOr<std::optional<std::string>> PopFrame(std::string* buffer, size_t max_frame_bytes);

}  // namespace slop::mcp::stdio_internal

#endif  // SLOP_MCP_CLIENT_STDIO_TRANSPORT_INTERNAL_H_
