#ifndef SLOP_MCP_CLIENT_H_
#define SLOP_MCP_CLIENT_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

#include "core/http_client.h"
#include "mcp/client/session.h"
#include "mcp/client/stdio_transport.h"
#include "mcp/protocol.h"
#include "mcp/types.h"

namespace slop::mcp {

inline constexpr size_t kMaxCatalogPages = 100;
inline constexpr size_t kMaxCatalogTools = 10000;

class Client {
 public:
  virtual ~Client() = default;
  virtual ProtocolRevision revision() const = 0;
  virtual absl::StatusOr<std::vector<Tool>> ListTools() = 0;
  virtual absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments,
                                                  absl::Duration timeout) = 0;
  virtual absl::StatusOr<ToolCallResult> ContinueToolCall(const std::string& name, const nlohmann::json& arguments,
                                                          const nlohmann::json& request_state,
                                                          absl::Duration timeout) = 0;
};

struct ClientOptions {
  SelectionPolicy selection = SelectionPolicy::kPreferLatest;
  ImplementationInfo client_info;
  nlohmann::json modern_capabilities = nlohmann::json::object();
  ClientCapabilities classic_capabilities;
  absl::Duration initialization_timeout = absl::Seconds(60);
  absl::Duration request_timeout = absl::Seconds(60);
};

absl::StatusOr<std::unique_ptr<v2025_11_25::Session>> ConnectClassicStreamableHttp(
    const StreamableHttpConfig& config, const v2025_11_25::InitializeOptions& options, HttpClient* http_client);

absl::StatusOr<std::unique_ptr<Client>> ConnectMcp(const StreamableHttpConfig& config, const ClientOptions& options,
                                                   HttpClient* http_client);

// Connects to a modern MCP server started as a local stdio child process. This
// connector never falls back to the classic protocol.
absl::StatusOr<std::unique_ptr<Client>> ConnectStdioMcp(StdioTransportOptions transport_options,
                                                        const ClientOptions& options);

}  // namespace slop::mcp

#endif  // SLOP_MCP_CLIENT_H_
