#include "mcp/runtime.h"

#include <cctype>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/status_macros.h"
#include "mcp/client.h"
#include "mcp/token_store.h"

namespace slop::mcp {
namespace {

std::string SanitizeNamePart(const std::string& value) {
  std::string sanitized;
  sanitized.reserve(value.size());
  for (char c : value) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (std::isalnum(ch) || c == '_' || c == '-') {
      sanitized.push_back(c);
    } else {
      sanitized.push_back('_');
    }
  }
  return sanitized.empty() ? "unnamed" : sanitized;
}

std::string ToolDescription(const ServerRegistryEntry& entry, const Tool& tool) {
  if (tool.description.has_value() && !tool.description->empty()) {
    return absl::StrCat("MCP ", entry.name, ": ", *tool.description);
  }
  if (tool.title.has_value() && !tool.title->empty()) {
    return absl::StrCat("MCP ", entry.name, ": ", *tool.title);
  }
  return absl::StrCat("MCP ", entry.name, " tool ", tool.name);
}

std::string AuthFailureHint(const ServerRegistryEntry& entry) {
  if (entry.auth == kAuthBearer) {
    return absl::StrCat("check the bearer token and re-run `std_slop mcp add ", entry.name, " --url ", entry.url,
                        " --auth bearer --token <token>`");
  }
  if (entry.auth == kAuthOAuth) {
    return absl::StrCat("run `std_slop mcp oauth-login ", entry.name, "` or `std_slop mcp oauth-refresh ", entry.name,
                        "`");
  }
  return "check the server configuration";
}

absl::Status WithAuthContext(const ServerRegistryEntry& entry, const absl::Status& status) {
  if (status.ok()) return status;
  if (absl::IsUnauthenticated(status)) {
    return absl::UnauthenticatedError(absl::StrCat("MCP request failed for server '", entry.name,
                                                   "': authentication failed; ", AuthFailureHint(entry)));
  }
  if (absl::IsPermissionDenied(status)) {
    const std::string hint = entry.auth == kAuthBearer ? "check bearer token permissions" : AuthFailureHint(entry);
    return absl::PermissionDeniedError(
        absl::StrCat("MCP request failed for server '", entry.name, "': permission denied; ", hint));
  }
  return status;
}

absl::StatusOr<StreamableHttpConfig> TransportConfigFromEntry(const ServerRegistryEntry& entry) {
  StreamableHttpConfig config;
  config.endpoint_url = entry.url;
  if (entry.auth == kAuthBearer || entry.auth == kAuthOAuth) {
    auto tokens = LoadOAuthTokens(entry.token_path);
    if (!tokens.ok()) {
      if (entry.auth == kAuthBearer) {
        return absl::UnauthenticatedError(absl::StrCat("MCP bearer token is missing or invalid for server '",
                                                       entry.name, "'; ", AuthFailureHint(entry)));
      }
      return absl::UnauthenticatedError(absl::StrCat("MCP OAuth token is missing or invalid for server '", entry.name,
                                                     "'; ", AuthFailureHint(entry)));
    }
    if (entry.auth == kAuthOAuth) {
      if (tokens->issuer.empty() || tokens->resource.empty() || tokens->issuer != entry.authorization_server_url ||
          tokens->resource != entry.url) {
        return absl::PermissionDeniedError(
            absl::StrCat("MCP OAuth token binding does not match server '", entry.name, "'"));
      }
      if (tokens->expires_at_unix_seconds != 0 && tokens->expires_at_unix_seconds <= std::time(nullptr)) {
        return absl::UnauthenticatedError(
            absl::StrCat("MCP OAuth token is expired for server '", entry.name, "'; ", AuthFailureHint(entry)));
      }
    }
    config.bearer_token = tokens->access_token;
  }
  return config;
}

ClientOptions ClientOptionsForRuntime(const RuntimeOptions& options) {
  ClientOptions client_options;
  client_options.selection = SelectionPolicy::kPreferLatest;
  client_options.client_info.name = options.client_name;
  client_options.client_info.version = options.client_version;
  return client_options;
}

class RealRuntimeSession : public RuntimeSession {
 public:
  explicit RealRuntimeSession(std::unique_ptr<Client> client) : client_(std::move(client)) {}

  absl::StatusOr<std::vector<Tool>> ListTools() override { return client_->ListTools(); }

  absl::StatusOr<ToolCallResult> CallTool(const std::string& name, const nlohmann::json& arguments) override {
    return client_->CallTool(name, arguments);
  }

 private:
  std::unique_ptr<Client> client_;
};

absl::StatusOr<std::unique_ptr<RuntimeSession>> RealSessionFactory(const ServerRegistryEntry& entry,
                                                                   HttpClient* http_client,
                                                                   const RuntimeOptions& options) {
  auto config = TransportConfigFromEntry(entry);
  if (!config.ok()) return config.status();
  auto client = ConnectMcp(*config, ClientOptionsForRuntime(options), http_client);
  if (!client.ok()) return WithAuthContext(entry, client.status());
  return std::make_unique<RealRuntimeSession>(std::move(*client));
}

}  // namespace

std::string RuntimeToolName(const std::string& server_name, const std::string& tool_name) {
  return absl::StrCat("mcp_", SanitizeNamePart(server_name), "_", SanitizeNamePart(tool_name));
}

absl::StatusOr<std::string> NormalizeToolCallResult(const ToolCallResult& result) {
  nlohmann::json content = nlohmann::json::array();
  for (const auto& item : result.content) {
    if (!item.is_object()) {
      return absl::InvalidArgumentError("MCP tool result content entries must be objects");
    }
    content.push_back(item);
  }
  nlohmann::json normalized = nlohmann::json::object();
  normalized["content"] = std::move(content);
  normalized["is_error"] = result.is_error;
  if (result.structured_content) {
    normalized["structured_content"] = *result.structured_content;
  }
  if (!result.meta.empty()) normalized["meta"] = result.meta;
  return json_dump(normalized);
}

RuntimeManager::RuntimeManager(Database* db, ToolExecutor* tool_executor, HttpClient* http_client,
                               RuntimeOptions options, SessionFactory session_factory)
    : db_(db),
      tool_executor_(tool_executor),
      http_client_(http_client),
      options_(std::move(options)),
      session_factory_(std::move(session_factory)) {}

absl::Status RuntimeManager::Start() {
  if (db_ == nullptr) return absl::InvalidArgumentError("Database cannot be null");
  if (tool_executor_ == nullptr) return absl::InvalidArgumentError("ToolExecutor cannot be null");
  if (http_client_ == nullptr) return absl::InvalidArgumentError("HttpClient cannot be null");
  if (!session_factory_) return absl::InvalidArgumentError("MCP session factory cannot be empty");

  RETURN_IF_ERROR(db_->Execute("DELETE FROM tools WHERE name LIKE 'mcp\\_%' ESCAPE '\\';"));
  const std::string registry_path = options_.registry_path.empty() ? DefaultRegistryPath() : options_.registry_path;
  auto entries = LoadServerRegistry(registry_path);
  if (!entries.ok()) {
    if (absl::IsNotFound(entries.status())) return absl::OkStatus();
    return entries.status();
  }

  for (const auto& entry : *entries) {
    if (!entry.enabled) continue;
    auto session = session_factory_(entry, http_client_, options_);
    if (!session.ok()) {
      LOG(WARNING) << "MCP server startup failed for " << entry.name << ": " << session.status();
      continue;
    }
    auto tools = (*session)->ListTools();
    if (!tools.ok()) {
      LOG(WARNING) << "MCP tool discovery failed for " << entry.name << ": " << WithAuthContext(entry, tools.status());
      continue;
    }
    catalog_cache_[entry.name] = *tools;
    std::unique_ptr<RuntimeSession> owned_session = std::move(*session);
    RuntimeSession* session_ptr = owned_session.get();
    RETURN_IF_ERROR(RegisterServerTools(entry, session_ptr, *tools));
    sessions_.push_back(ActiveSession{entry, std::move(owned_session)});
  }
  return absl::OkStatus();
}

absl::Status RuntimeManager::RefreshCatalogs() {
  struct Candidate {
    std::string runtime_name;
    ActiveSession* active;
    Tool tool;
  };
  std::vector<Candidate> candidates;
  absl::flat_hash_set<std::string> names;
  for (ActiveSession& active : sessions_) {
    auto tools_or = active.session->ListTools();
    if (!tools_or.ok()) {
      const auto cached = catalog_cache_.find(active.entry.name);
      if (cached == catalog_cache_.end()) return tools_or.status();
      LOG(WARNING) << "Using cached MCP tool catalog for " << active.entry.name << ": " << tools_or.status();
      tools_or = cached->second;
    } else {
      catalog_cache_[active.entry.name] = *tools_or;
    }
    for (Tool& tool : *tools_or) {
      const std::string runtime_name = RuntimeToolName(active.entry.name, tool.name);
      if (runtime_name.size() > kMaxRuntimeToolNameLength) {
        return absl::FailedPreconditionError(
            absl::StrCat("MCP runtime tool name is too long for providers: ", runtime_name));
      }
      if (!names.insert(runtime_name).second) {
        return absl::FailedPreconditionError(absl::StrCat("Duplicate MCP runtime tool name: ", runtime_name));
      }
      candidates.push_back({runtime_name, &active, std::move(tool)});
    }
  }

  RETURN_IF_ERROR(db_->Execute("BEGIN IMMEDIATE"));
  for (const auto& [runtime_name, route] : routes_) {
    (void)route;
    const absl::Status status = db_->Execute("DELETE FROM tools WHERE name = ?", runtime_name);
    if (!status.ok()) {
      (void)db_->Execute("ROLLBACK");
      return status;
    }
  }
  for (const Candidate& candidate : candidates) {
    const absl::Status status = db_->RegisterTool(
        Database::Tool{candidate.runtime_name, ToolDescription(candidate.active->entry, candidate.tool),
                       json_dump(candidate.tool.input_schema), true, 0, true});
    if (!status.ok()) {
      (void)db_->Execute("ROLLBACK");
      return status;
    }
  }
  const absl::Status commit = db_->Execute("COMMIT");
  if (!commit.ok()) {
    (void)db_->Execute("ROLLBACK");
    return commit;
  }

  for (const auto& [runtime_name, route] : routes_) {
    (void)route;
    tool_executor_->UnregisterTool(runtime_name);
  }
  routes_.clear();
  for (const Candidate& candidate : candidates) {
    routes_[candidate.runtime_name] =
        ToolRoute{candidate.active->session.get(), candidate.active->entry.name, candidate.active->entry.url,
                  candidate.active->entry.auth, candidate.tool.name};
    tool_executor_->RegisterTool(candidate.runtime_name,
                                 [this, runtime_name = candidate.runtime_name](const nlohmann::json& args, auto) {
                                   return ExecuteRuntimeTool(runtime_name, args);
                                 });
  }
  return absl::OkStatus();
}

absl::Status RuntimeManager::RegisterServerTools(const ServerRegistryEntry& entry, RuntimeSession* session,
                                                 const std::vector<Tool>& tools) {
  struct Candidate {
    std::string runtime_name;
    const Tool* tool;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(tools.size());
  absl::flat_hash_set<std::string> server_tool_names;
  for (const Tool& tool : tools) {
    const std::string runtime_name = RuntimeToolName(entry.name, tool.name);
    if (runtime_name.size() > kMaxRuntimeToolNameLength) {
      return absl::FailedPreconditionError(
          absl::StrCat("MCP runtime tool name is too long for providers: ", runtime_name));
    }
    if (routes_.contains(runtime_name) || !server_tool_names.insert(runtime_name).second) {
      return absl::FailedPreconditionError(absl::StrCat("Duplicate MCP runtime tool name: ", runtime_name));
    }
    candidates.push_back({runtime_name, &tool});
  }

  RETURN_IF_ERROR(db_->Execute("BEGIN IMMEDIATE"));
  for (const Candidate& candidate : candidates) {
    const absl::Status status =
        db_->RegisterTool(Database::Tool{candidate.runtime_name, ToolDescription(entry, *candidate.tool),
                                         json_dump(candidate.tool->input_schema), true, 0, true});
    if (!status.ok()) {
      (void)db_->Execute("ROLLBACK");
      return status;
    }
  }
  const absl::Status commit = db_->Execute("COMMIT");
  if (!commit.ok()) {
    (void)db_->Execute("ROLLBACK");
    return commit;
  }

  for (const Candidate& candidate : candidates) {
    routes_[candidate.runtime_name] = ToolRoute{session, entry.name, entry.url, entry.auth, candidate.tool->name};
    tool_executor_->RegisterTool(candidate.runtime_name,
                                 [this, runtime_name = candidate.runtime_name](const nlohmann::json& args, auto) {
                                   return ExecuteRuntimeTool(runtime_name, args);
                                 });
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> RuntimeManager::ExecuteRuntimeTool(const std::string& runtime_name,
                                                               const nlohmann::json& args) {
  const auto it = routes_.find(runtime_name);
  if (it == routes_.end()) return absl::NotFoundError(absl::StrCat("MCP runtime tool not found: ", runtime_name));
  if (!args.is_object()) return absl::InvalidArgumentError("MCP tool arguments must be an object");
  LOG(INFO) << "Calling MCP tool " << runtime_name << " on server " << it->second.server_name;
  auto result = it->second.session->CallTool(it->second.remote_tool_name, args);
  if (!result.ok()) {
    ServerRegistryEntry entry;
    entry.name = it->second.server_name;
    entry.url = it->second.server_url;
    entry.auth = it->second.auth_mode;
    return WithAuthContext(entry, result.status());
  }
  return NormalizeToolCallResult(*result);
}

absl::StatusOr<std::unique_ptr<RuntimeManager>> StartMcpRuntime(Database* db, ToolExecutor* tool_executor,
                                                                HttpClient* http_client, RuntimeOptions options) {
  auto manager =
      std::make_unique<RuntimeManager>(db, tool_executor, http_client, std::move(options), RealSessionFactory);
  RETURN_IF_ERROR(manager->Start());
  return manager;
}

}  // namespace slop::mcp
