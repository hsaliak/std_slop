#ifndef SLOP_APP_MCP_COMMANDS_H_
#define SLOP_APP_MCP_COMMANDS_H_

#include <iosfwd>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "core/http_client.h"

namespace slop {

absl::Status RunMcpCommand(const std::vector<std::string>& args, HttpClient* http_client, std::istream* in,
                           std::ostream* out, std::ostream* err);

}  // namespace slop

#endif  // SLOP_APP_MCP_COMMANDS_H_
