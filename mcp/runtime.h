#ifndef SLOP_MCP_RUNTIME_H_
#define SLOP_MCP_RUNTIME_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "core/database.h"
#include "core/http_client.h"
#include "mcp/registry.h"
#include "mcp/types.h"
#include "nlohmann/json.hpp"
#include "tools/tool_executor.h"

namespace slop::mcp {

class RuntimeSession {
 public:
  virtual ~RuntimeSession() = default;
  virtual absl::StatusOr<std::vector<Tool>> ListTools() = 0;
  virtual absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) = 0;
};

struct RuntimeOptions {
  std::string client_name = "std_slop";
  std::string client_version = "mcp-runtime";
  std::string registry_path;
};

absl::StatusOr<std::string> NormalizeToolCallResult(const ToolCallResult& result);
std::string RuntimeToolName(const std::string& server_name, const std::string& tool_name);

class RuntimeManager {
 public:
  using SessionFactory = std::function<absl::StatusOr<std::unique_ptr<RuntimeSession>>(const ServerRegistryEntry& entry,
                                                                                       HttpClient* http_client,
                                                                                       const RuntimeOptions& options)>;

  RuntimeManager(Database* db, ToolExecutor* tool_executor, HttpClient* http_client, RuntimeOptions options,
                 SessionFactory session_factory);

  absl::Status Start();
  size_t active_server_count() const { return sessions_.size(); }

 private:
  struct ToolRoute {
    RuntimeSession* session = nullptr;
    std::string server_name;
    std::string remote_tool_name;
  };

  absl::Status RegisterServerTools(const ServerRegistryEntry& entry, RuntimeSession* session,
                                   const std::vector<Tool>& tools);
  absl::StatusOr<std::string> ExecuteRuntimeTool(const std::string& runtime_name, const nlohmann::json& args);

  Database* db_;
  ToolExecutor* tool_executor_;
  HttpClient* http_client_;
  RuntimeOptions options_;
  SessionFactory session_factory_;
  std::vector<std::unique_ptr<RuntimeSession>> sessions_;
  absl::flat_hash_map<std::string, ToolRoute> routes_;
};

absl::StatusOr<std::unique_ptr<RuntimeManager>> StartMcpRuntime(Database* db, ToolExecutor* tool_executor,
                                                                HttpClient* http_client,
                                                                RuntimeOptions options = RuntimeOptions());

}  // namespace slop::mcp

#endif  // SLOP_MCP_RUNTIME_H_
