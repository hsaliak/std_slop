#ifndef SLOP_MCP_PROTOCOL_H_
#define SLOP_MCP_PROTOCOL_H_

#include <optional>
#include <string>

#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

inline constexpr absl::string_view kJsonRpcVersion = "2.0";
inline constexpr absl::string_view kClassicProtocolVersion = "2025-11-25";
inline constexpr absl::string_view kModernProtocolVersion = "2026-07-28";
inline constexpr absl::string_view kProtocolVersionHeader = "MCP-Protocol-Version";
inline constexpr absl::string_view kSessionIdHeader = "Mcp-Session-Id";

enum class ProtocolRevision { k2025_11_25, k2026_07_28 };
enum class SelectionPolicy { kLatestOnly, kClassicOnly, kPreferLatest };
enum class ExecutionCertainty { kNotExecuted, kMayHaveExecuted, kExecuted };

struct ProtocolFailure {
  std::string message;
  std::optional<int> json_rpc_code;
  nlohmann::json json_rpc_data = nlohmann::json::object();
  std::optional<long> http_status;
  ExecutionCertainty execution = ExecutionCertainty::kNotExecuted;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_PROTOCOL_H_
