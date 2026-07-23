#ifndef SLOP_MCP_REGISTRY_H_
#define SLOP_MCP_REGISTRY_H_

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace slop::mcp {

struct ServerRegistryEntry {
  std::string name;
  std::string url;
  std::string auth = "none";
  bool enabled = true;
  std::vector<std::string> scopes;
  std::string token_path;
  std::string client_id;
  std::string resource_metadata_url;
  std::string authorization_server_url;
  std::string authorization_endpoint;
  std::string token_endpoint;
};

absl::Status ValidateServerRegistryEntry(const ServerRegistryEntry& entry);
std::string DefaultRegistryPath();
std::string DefaultTokenPath(const std::string& server_name);
absl::StatusOr<std::vector<ServerRegistryEntry>> LoadServerRegistry(const std::string& path);
absl::Status SaveServerRegistry(const std::string& path, const std::vector<ServerRegistryEntry>& entries);
absl::Status UpsertServerRegistryEntry(const std::string& path, const ServerRegistryEntry& entry);
absl::Status RemoveServerRegistryEntry(const std::string& path, const std::string& name);

}  // namespace slop::mcp

#endif  // SLOP_MCP_REGISTRY_H_
