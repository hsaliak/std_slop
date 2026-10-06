#include "mcp/gateway/catalog.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/gateway/config.h"
#include "mcp/json_schema.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCatalogTools = 1000;
constexpr std::size_t kMaxCatalogBytes = 1024 * 1024;
constexpr std::size_t kMaxToolNameBytes = 256;

nlohmann::json ToolSummary(const std::string& alias, const Tool& tool) {
  nlohmann::json summary = {{"server", alias}, {"name", tool.name}};
  if (tool.description.has_value()) summary["description"] = *tool.description;
  if (tool.title.has_value()) summary["title"] = *tool.title;
  return summary;
}

}  // namespace

absl::StatusOr<Catalog> Catalog::Create(std::vector<DownstreamClient> servers) {
  if (servers.size() > 8) return absl::InvalidArgumentError("catalog exceeds server limit");
  Catalog catalog(std::move(servers));
  std::set<std::string> aliases;
  for (std::size_t server_index = 0; server_index < catalog.servers_.size(); ++server_index) {
    DownstreamClient& server = catalog.servers_[server_index];
    if (!IsSafeServerAlias(server.alias)) return absl::InvalidArgumentError("catalog contains an unsafe server alias");
    if (!aliases.insert(server.alias).second)
      return absl::InvalidArgumentError("catalog server aliases must be unique");
    if (server.client == nullptr) return absl::InvalidArgumentError("catalog server client must not be null");
    std::set<std::string> grants(server.allow_tools.begin(), server.allow_tools.end());
    auto tools = server.client->ListTools();
    if (!tools.ok()) return tools.status();
    std::set<std::string> discovered;
    for (const Tool& tool : *tools) {
      if (tool.name.empty() || tool.name.size() > kMaxToolNameBytes || !discovered.insert(tool.name).second) {
        return absl::InvalidArgumentError("downstream catalog has an empty, overlong, or duplicate tool name");
      }
      if (!grants.empty() && grants.find(tool.name) == grants.end()) continue;
      absl::Status schema_status = CheckJsonSchema(tool.input_schema);
      if (!schema_status.ok())
        return absl::InvalidArgumentError(
            absl::StrCat("invalid schema for ", server.alias, "/", tool.name, ": ", schema_status.message()));
      catalog.routes_.emplace(std::make_pair(server.alias, tool.name), Route{server.client.get(), tool});
      if (catalog.routes_.size() > kMaxCatalogTools) return absl::ResourceExhaustedError("catalog exceeds tool limit");
    }
  }
  const std::string serialized = json_dump(catalog.PublicCatalog());
  if (serialized.size() > kMaxCatalogBytes) return absl::ResourceExhaustedError("catalog metadata exceeds size limit");
  return catalog;
}

absl::StatusOr<ToolCallResult> Catalog::Call(const std::string& alias, const std::string& tool,
                                             const nlohmann::json& arguments,
                                             std::chrono::steady_clock::time_point deadline) {
  const Route* route = FindRoute(alias, tool);
  if (route == nullptr) return absl::PermissionDeniedError("tool is not in the authorized catalog");
  if (!arguments.is_object()) return absl::InvalidArgumentError("tool arguments must be an object");
  absl::Status schema_status = ValidateJsonSchema(route->definition.input_schema, arguments);
  if (!schema_status.ok()) return schema_status;
  if (route->client == nullptr) return absl::FailedPreconditionError("downstream client is not available");
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return absl::DeadlineExceededError("tool deadline expired during argument validation");
  }
  return route->client->CallTool(tool, arguments, absl::FromChrono(remaining));
}

absl::StatusOr<nlohmann::json> Catalog::Help(const std::string& alias, const std::string& tool) const {
  nlohmann::json summaries = nlohmann::json::array();
  for (const auto& [key, route] : routes_) {
    if (!alias.empty() && key.first != alias) continue;
    if (!tool.empty() && key.second != tool) continue;
    nlohmann::json summary = ToolSummary(key.first, route.definition);
    if (!tool.empty()) {
      summary["inputSchema"] = route.definition.input_schema;
      summary["result"] = {{"content", "array"}, {"structuredContent", "optional JSON value"}, {"isError", "boolean"}};
    }
    summaries.push_back(std::move(summary));
  }
  if ((!alias.empty() || !tool.empty()) && summaries.empty()) {
    return absl::NotFoundError("tool or server is not in the authorized catalog");
  }
  nlohmann::json result = {{"servers", nlohmann::json::array()}, {"tools", summaries}};
  return result;
}

std::vector<std::string> Catalog::server_aliases() const {
  std::vector<std::string> aliases;
  aliases.reserve(servers_.size());
  for (const DownstreamClient& server : servers_) aliases.push_back(server.alias);
  return aliases;
}

std::unique_ptr<Client> Catalog::TakeClient(const std::string& alias) {
  for (DownstreamClient& server : servers_) {
    if (server.alias == alias) return std::move(server.client);
  }
  return nullptr;
}

nlohmann::json Catalog::PublicCatalog() const {
  std::map<std::string, nlohmann::json> server_entries;
  for (const auto& [key, route] : routes_) {
    auto inserted =
        server_entries.emplace(key.first, nlohmann::json{{"alias", key.first}, {"tools", nlohmann::json::array()}});
    inserted.first->second["tools"].push_back(ToolSummary(key.first, route.definition));
  }
  nlohmann::json result = nlohmann::json::array();
  for (auto& [alias, server] : server_entries) {
    for (auto& item : server["tools"]) {
      const Route* route = FindRoute(alias, json_get_or(item, "name", std::string()));
      if (route != nullptr) item["inputSchema"] = route->definition.input_schema;
    }
    result.push_back(std::move(server));
  }
  return result;
}

const Catalog::Route* Catalog::FindRoute(const std::string& alias, const std::string& tool) const {
  auto it = routes_.find({alias, tool});
  return it == routes_.end() ? nullptr : &it->second;
}

}  // namespace slop::mcp::gateway
