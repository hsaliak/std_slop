#ifndef SLOP_RPC_SERVER_CONFIG_H_
#define SLOP_RPC_SERVER_CONFIG_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "app/runtime_bootstrap.h"
#include "core/config.h"
#include "rpc/server_config.pb.h"

namespace slop::rpc::v1 {

struct ServerRuntimeConfig {
  std::string listen_addr;
  std::string db_path;
  ServerConfig proto;
  RuntimeBootstrapOptions runtime_options;
  std::vector<std::string> active_skills;
  std::optional<int> context_window;
  bool disable_ask_user = true;
  bool allow_request_model_override = false;
  bool allow_request_skill_override = false;
  bool allow_request_context_window_override = false;
  int max_context_window = 0;
};

ServerConfig ApplyServerConfigDefaults(const ServerConfig& config);
absl::Status ValidateServerConfig(const ServerConfig& config);

absl::StatusOr<ServerRuntimeConfig> BuildServerRuntimeConfig(const ServerConfig& config);

absl::StatusOr<ServerConfig> LoadServerConfigTextproto(const std::string& path);

absl::StatusOr<ServerRuntimeConfig> LoadServerRuntimeConfig(const std::string& path);

absl::StatusOr<std::vector<slop::LlmToolSpecializationConfig>> ConvertLlmToolSpecializations(
    const ServerConfig& config);

}  // namespace slop::rpc::v1

#endif  // SLOP_RPC_SERVER_CONFIG_H_
