#ifndef SLOP_MCP_PROTOCOL_H_
#define SLOP_MCP_PROTOCOL_H_

#include "absl/strings/string_view.h"

namespace slop::mcp {

inline constexpr absl::string_view kJsonRpcVersion = "2.0";
inline constexpr absl::string_view kLatestProtocolVersion = "2025-11-25";
inline constexpr absl::string_view kProtocolVersionHeader = "MCP-Protocol-Version";
inline constexpr absl::string_view kSessionIdHeader = "Mcp-Session-Id";

}  // namespace slop::mcp

#endif  // SLOP_MCP_PROTOCOL_H_
