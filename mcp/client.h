#ifndef SLOP_MCP_CLIENT_H_
#define SLOP_MCP_CLIENT_H_

#include <memory>

#include "absl/status/statusor.h"
#include "core/http_client.h"
#include "mcp/session.h"
#include "mcp/types.h"

namespace slop::mcp {

absl::StatusOr<std::unique_ptr<Session>> ConnectStreamableHttp(const StreamableHttpConfig& config,
                                                               const InitializeOptions& options,
                                                               HttpClient* http_client);

}  // namespace slop::mcp

#endif  // SLOP_MCP_CLIENT_H_
