#ifndef SLOP_MCP_SERVER_STDIO_H_
#define SLOP_MCP_SERVER_STDIO_H_

#include <cstddef>
#include <iosfwd>

#include "absl/status/status.h"

#include "mcp/server/server.h"

namespace slop::mcp::server {

struct StdioOptions {
  // Maximum bytes before LF, including an optional CR. Must be nonzero.
  size_t max_input_bytes = 1024 * 1024;
};

// Runs synchronous newline-delimited JSON-RPC using borrowed streams. No TTY,
// process launch, stream closing or global stdin/stdout manipulation is done.
// Use separate buffers and streams with exceptions disabled. Diagnostics are
// best-effort and never sent to output. Protocol errors are normal responses.
// Clean EOF succeeds; unterminated input, oversized lines and I/O failures stop
// the loop with an error. Each response is flushed before reading another line.
// An output failure may occur after a handler ran; callers must not retry it
// blindly. Handler-produced responses are not size-limited by this transport.
absl::Status RunStdio(const Server& server, std::istream& input, std::ostream& output, std::ostream& diagnostics,
                      StdioOptions options = {});

}  // namespace slop::mcp::server

#endif  // SLOP_MCP_SERVER_STDIO_H_
