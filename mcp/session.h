#ifndef SLOP_MCP_SESSION_H_
#define SLOP_MCP_SESSION_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "mcp/transport.h"
#include "mcp/types.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

struct InitializeOptions {
  ImplementationInfo client_info;
  ClientCapabilities capabilities;
  absl::Duration request_timeout = absl::Seconds(60);
};

class Session {
 public:
  explicit Session(std::unique_ptr<Transport> transport);

  absl::Status Initialize(const InitializeOptions& options);
  absl::Status Close();
  absl::Status Ping();
  std::vector<ServerNotification> DrainNotifications();
  absl::StatusOr<std::vector<Tool>> ListTools();
  absl::StatusOr<ToolCallResult> CallTool(absl::string_view name, const nlohmann::json& arguments);
  absl::StatusOr<std::vector<Resource>> ListResources();
  absl::StatusOr<ResourceReadResult> ReadResource(absl::string_view uri);
  absl::StatusOr<std::vector<Prompt>> ListPrompts();
  absl::StatusOr<PromptGetResult> GetPrompt(absl::string_view name, const nlohmann::json& arguments);

  const ServerCapabilities& server_capabilities() const { return server_capabilities_; }
  absl::string_view protocol_version() const { return protocol_version_; }
  bool initialized() const { return state_ == State::kInitialized; }

 private:
  enum class State { kCreated, kStarted, kInitialized, kClosed };

  absl::StatusOr<nlohmann::json> SendRequest(absl::string_view method, const nlohmann::json& params,
                                             absl::Duration timeout);
  absl::Status SendNotification(absl::string_view method, const nlohmann::json& params = nullptr);
  int64_t NextRequestId();

  static nlohmann::json BuildClientCapabilities(const ClientCapabilities& capabilities);
  static absl::StatusOr<ServerCapabilities> ParseServerCapabilities(const nlohmann::json& capabilities);
  absl::Status ParseInitializeResult(const nlohmann::json& result);
  static absl::StatusOr<Tool> ParseTool(const nlohmann::json& tool);
  static absl::StatusOr<ToolCallResult> ParseToolCallResult(const nlohmann::json& result);
  static absl::StatusOr<Resource> ParseResource(const nlohmann::json& resource);
  static absl::StatusOr<ResourceReadResult> ParseResourceReadResult(const nlohmann::json& result);
  static absl::StatusOr<Prompt> ParsePrompt(const nlohmann::json& prompt);
  static absl::StatusOr<PromptGetResult> ParsePromptGetResult(const nlohmann::json& result);
  absl::Status HandleNotification(const nlohmann::json& message);

  std::unique_ptr<Transport> transport_;
  State state_ = State::kCreated;
  int64_t next_request_id_ = 1;
  std::string protocol_version_;
  ServerCapabilities server_capabilities_;
  InitializeOptions options_;
  std::vector<ServerNotification> notifications_;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_SESSION_H_
