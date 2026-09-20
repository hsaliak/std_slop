#ifndef SLOP_MCP_CLIENT_H_
#define SLOP_MCP_CLIENT_H_

#include <memory>

#include "absl/status/statusor.h"

#include "core/http_client.h"
#include "mcp/session.h"
#include "mcp/types.h"

namespace slop::mcp {

absl::StatusOr<std::unique_ptr<v2025_11_25::Session>> ConnectClassicStreamableHttp(
    const StreamableHttpConfig& config, const v2025_11_25::InitializeOptions& options, HttpClient* http_client);

}  // namespace slop::mcp

#endif  // SLOP_MCP_CLIENT_H_
