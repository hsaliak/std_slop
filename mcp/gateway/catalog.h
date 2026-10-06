#ifndef SLOP_MCP_GATEWAY_CATALOG_H_
#define SLOP_MCP_GATEWAY_CATALOG_H_

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

#include "mcp/client/client.h"
#include "mcp/types.h"

namespace slop::mcp::gateway {

struct DownstreamClient {
  std::string alias;
  std::vector<std::string> allow_tools;
  std::unique_ptr<Client> client;
};

class Catalog {
 public:
  static absl::StatusOr<Catalog> Create(std::vector<DownstreamClient> servers);

  Catalog(Catalog&&) = default;
  Catalog& operator=(Catalog&&) = default;
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  absl::StatusOr<ToolCallResult> Call(const std::string& alias, const std::string& tool,
                                      const nlohmann::json& arguments, std::chrono::steady_clock::time_point deadline);
  absl::StatusOr<nlohmann::json> Help(const std::string& alias, const std::string& tool) const;
  nlohmann::json PublicCatalog() const;
  std::vector<std::string> server_aliases() const;
  std::unique_ptr<Client> TakeClient(const std::string& alias);
  std::size_t size() const { return routes_.size(); }

 private:
  struct Route {
    Client* client;
    Tool definition;
  };

  explicit Catalog(std::vector<DownstreamClient> servers) : servers_(std::move(servers)) {}
  const Route* FindRoute(const std::string& alias, const std::string& tool) const;

  std::vector<DownstreamClient> servers_;
  std::map<std::pair<std::string, std::string>, Route> routes_;
};

}  // namespace slop::mcp::gateway

#endif  // SLOP_MCP_GATEWAY_CATALOG_H_
