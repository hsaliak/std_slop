#ifndef SLOP_MCP_HTTP_HEADERS_H_
#define SLOP_MCP_HTTP_HEADERS_H_

#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

namespace slop::mcp {

absl::Status ValidateExtraHeaders(const absl::flat_hash_map<std::string, std::string>& headers);

}  // namespace slop::mcp

#endif  // SLOP_MCP_HTTP_HEADERS_H_
