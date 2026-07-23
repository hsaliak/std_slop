#include "app/mcp_commands.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "mcp/oauth_client.h"
#include "mcp/oauth_discovery.h"
#include "mcp/registry.h"
#include "mcp/token_store.h"

namespace slop {
namespace {

absl::Status Usage() { return absl::InvalidArgumentError("usage: std_slop mcp add|remove|list|login|refresh|logout ..."); }

std::string ValueAfter(const std::vector<std::string>& args, const std::string& flag) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return "";
}

std::vector<std::string> ValuesAfter(const std::vector<std::string>& args, const std::string& flag) {
  std::vector<std::string> values;
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) values.push_back(args[i + 1]);
  }
  return values;
}

absl::Status ValidateFlags(const std::vector<std::string>& args, const std::vector<std::string>& allowed_repeat,
                           const std::vector<std::string>& allowed_single) {
  for (size_t i = 3; i < args.size();) {
    if (args[i].rfind("--", 0) != 0) return absl::InvalidArgumentError("Unexpected MCP command argument");
    const bool is_repeat = std::find(allowed_repeat.begin(), allowed_repeat.end(), args[i]) != allowed_repeat.end();
    const bool is_single = std::find(allowed_single.begin(), allowed_single.end(), args[i]) != allowed_single.end();
    if (!is_repeat && !is_single) return absl::InvalidArgumentError(absl::StrCat("Unknown MCP flag: ", args[i]));
    if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
      return absl::InvalidArgumentError(absl::StrCat("MCP flag missing value: ", args[i]));
    }
    if (is_single && std::count(args.begin(), args.end(), args[i]) > 1) {
      return absl::InvalidArgumentError(absl::StrCat("Duplicate MCP flag: ", args[i]));
    }
    i += 2;
  }
  return absl::OkStatus();
}

absl::StatusOr<mcp::ServerRegistryEntry> FindEntry(const std::string& name) {
  auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
  if (!entries.ok()) return entries.status();
  for (const auto& entry : *entries) {
    if (entry.name == name) return entry;
  }
  return absl::NotFoundError(absl::StrCat("MCP server not found: ", name));
}

mcp::OAuthClientConfig ConfigFromEntry(const mcp::ServerRegistryEntry& entry) {
  mcp::OAuthClientConfig config;
  config.client_id = entry.client_id;
  config.authorization_endpoint = entry.authorization_endpoint;
  config.token_endpoint = entry.token_endpoint;
  config.scopes = entry.scopes;
  return config;
}

}  // namespace

absl::Status RunMcpCommand(const std::vector<std::string>& args, HttpClient* http_client, std::istream* in,
                           std::ostream* out, std::ostream* err) {
  if (args.size() < 2 || args[0] != "mcp") return Usage();
  const std::string& command = args[1];
  if (command == "add") {
    if (args.size() < 3) return Usage();
    const absl::Status flag_status = ValidateFlags(
        args, {"--scope"}, {"--url", "--auth", "--client-id", "--token-path", "--authorization-endpoint", "--token-endpoint"});
    if (!flag_status.ok()) return flag_status;
    mcp::ServerRegistryEntry entry;
    entry.name = args[2];
    entry.url = ValueAfter(args, "--url");
    entry.auth = ValueAfter(args, "--auth");
    if (entry.auth.empty()) entry.auth = "none";
    entry.client_id = ValueAfter(args, "--client-id");
    entry.token_path = ValueAfter(args, "--token-path");
    if (entry.token_path.empty()) entry.token_path = mcp::DefaultTokenPath(entry.name);
    entry.authorization_endpoint = ValueAfter(args, "--authorization-endpoint");
    entry.token_endpoint = ValueAfter(args, "--token-endpoint");
    entry.scopes = ValuesAfter(args, "--scope");
    if (entry.auth == "oauth" && entry.client_id.empty()) {
      return absl::InvalidArgumentError("MCP OAuth server requires client_id");
    }
    if (entry.auth == "oauth" && (entry.authorization_endpoint.empty() || entry.token_endpoint.empty())) {
      if (!entry.authorization_endpoint.empty() || !entry.token_endpoint.empty()) {
        return absl::InvalidArgumentError(
            "MCP OAuth discovery requires both authorization_endpoint and token_endpoint to be omitted");
      }
      if (http_client == nullptr) return absl::InvalidArgumentError("MCP OAuth discovery requires an HTTP client");
      auto discovery = mcp::DiscoverOAuthEndpoints(http_client, entry.url);
      if (!discovery.ok()) return discovery.status();
      entry.authorization_endpoint = discovery->authorization_endpoint;
      entry.token_endpoint = discovery->token_endpoint;
      entry.resource_metadata_url = discovery->resource_metadata_url;
      entry.authorization_server_url = discovery->authorization_server_url;
    }
    const absl::Status status = mcp::UpsertServerRegistryEntry(mcp::DefaultRegistryPath(), entry);
    if (!status.ok()) return status;
    *out << "MCP server saved: " << entry.name << "\n";
    return absl::OkStatus();
  }
  if (command == "remove") {
    if (args.size() != 3) return Usage();
    auto entry = FindEntry(args[2]);
    if (entry.ok()) (void)mcp::DeleteOAuthTokens(entry->token_path);
    const absl::Status status = mcp::RemoveServerRegistryEntry(mcp::DefaultRegistryPath(), args[2]);
    if (!status.ok()) return status;
    *out << "MCP server removed: " << args[2] << "\n";
    return absl::OkStatus();
  }
  if (command == "list") {
    auto entries = mcp::LoadServerRegistry(mcp::DefaultRegistryPath());
    if (!entries.ok()) return entries.status();
    for (const auto& entry : *entries) {
      *out << entry.name << "\t" << entry.auth << "\t" << (entry.enabled ? "enabled" : "disabled") << "\t"
           << entry.url << "\n";
    }
    return absl::OkStatus();
  }
  if (command == "logout") {
    if (args.size() != 3) return Usage();
    auto entry = FindEntry(args[2]);
    if (!entry.ok()) return entry.status();
    const absl::Status status = mcp::DeleteOAuthTokens(entry->token_path);
    if (!status.ok() && status.code() != absl::StatusCode::kNotFound) return status;
    *out << "MCP token deleted: " << args[2] << "\n";
    return absl::OkStatus();
  }
  if (command == "login") {
    if (args.size() != 3 || http_client == nullptr || in == nullptr) return Usage();
    auto entry = FindEntry(args[2]);
    if (!entry.ok()) return entry.status();
    auto session = mcp::StartPkceAuthorization(ConfigFromEntry(*entry));
    if (!session.ok()) return session.status();
    *out << session->authorization_url << "\nPaste callback URL: ";
    std::string callback;
    std::getline(*in, callback);
    auto code = mcp::ExtractAuthorizationCodeFromCallback(callback, session->state);
    if (!code.ok()) return code.status();
    auto tokens = mcp::ExchangeAuthorizationCode(http_client, ConfigFromEntry(*entry), *code, session->code_verifier);
    if (!tokens.ok()) return tokens.status();
    const absl::Status status = mcp::SaveOAuthTokens(entry->token_path, *tokens);
    if (!status.ok()) return status;
    *out << "MCP login complete: " << entry->name << "\n";
    return absl::OkStatus();
  }
  if (command == "refresh") {
    if (args.size() != 3 || http_client == nullptr) return Usage();
    auto entry = FindEntry(args[2]);
    if (!entry.ok()) return entry.status();
    auto existing = mcp::LoadOAuthTokens(entry->token_path);
    if (!existing.ok()) return existing.status();
    auto tokens = mcp::RefreshOAuthToken(http_client, ConfigFromEntry(*entry), existing->refresh_token);
    if (!tokens.ok()) return tokens.status();
    if (tokens->refresh_token.empty()) tokens->refresh_token = existing->refresh_token;
    const absl::Status status = mcp::SaveOAuthTokens(entry->token_path, *tokens);
    if (!status.ok()) return status;
    *out << "MCP token refreshed: " << entry->name << "\n";
    return absl::OkStatus();
  }
  if (err != nullptr) *err << "Unknown MCP command\n";
  return Usage();
}

}  // namespace slop
