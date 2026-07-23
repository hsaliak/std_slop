#include "mcp/registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "ini/ini_parser.h"

namespace slop::mcp {
namespace {

constexpr absl::string_view kServerSectionPrefix = "server.";

bool IsValidServerName(absl::string_view name) {
  if (name.empty()) return false;
  for (const char c : name) {
    if (!(absl::ascii_isalnum(c) || c == '_' || c == '-')) return false;
  }
  return true;
}

bool HasIniControlCharacter(absl::string_view value) {
  for (const char c : value) {
    if (c == '\n' || c == '\r' || c == '[' || c == ']' || c == ';' || c == '#') return true;
  }
  return false;
}

absl::StatusOr<bool> ParseBool(absl::string_view value) {
  if (value == "true") return true;
  if (value == "false") return false;
  return absl::InvalidArgumentError("MCP server enabled must be true or false");
}

std::vector<std::string> ParseScopes(absl::string_view value) {
  std::vector<std::string> scopes;
  for (const absl::string_view part : absl::StrSplit(value, ' ', absl::SkipEmpty())) {
    scopes.emplace_back(part);
  }
  return scopes;
}

}  // namespace

absl::Status ValidateServerRegistryEntry(const ServerRegistryEntry& entry) {
  if (!IsValidServerName(entry.name)) {
    return absl::InvalidArgumentError("MCP server name must contain only letters, digits, hyphens, or underscores");
  }
  if (!(absl::StartsWith(entry.url, "https://") || absl::StartsWith(entry.url, "http://"))) {
    return absl::InvalidArgumentError("MCP server URL must use http or https");
  }
  if (entry.auth != "none" && entry.auth != "bearer" && entry.auth != "oauth") {
    return absl::InvalidArgumentError("MCP server auth must be none, bearer, or oauth");
  }
  if (entry.auth == "bearer" && entry.token_path.empty()) {
    return absl::InvalidArgumentError("MCP bearer server requires token_path");
  }
  if (HasIniControlCharacter(entry.url) || HasIniControlCharacter(entry.token_path)) {
    return absl::InvalidArgumentError("MCP server fields contain unsafe INI control characters");
  }
  for (const std::string& scope : entry.scopes) {
    if (scope.empty() || HasIniControlCharacter(scope) || absl::StrContains(scope, " ")) {
      return absl::InvalidArgumentError("MCP scopes must be non-empty single INI-safe tokens");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<ServerRegistryEntry>> LoadServerRegistry(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error) return absl::UnavailableError(absl::StrCat("Failed to access MCP registry: ", error.message()));
    return std::vector<ServerRegistryEntry>{};
  }
  std::ifstream file(path);
  if (!file.is_open()) return absl::UnavailableError(absl::StrCat("Failed to open MCP registry: ", path));
  const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const IniConfig config = ParseIni(content);
  std::vector<ServerRegistryEntry> entries;
  for (const auto& [section_name, section] : config) {
    if (!absl::StartsWith(section_name, kServerSectionPrefix)) continue;
    ServerRegistryEntry entry;
    entry.name = section_name.substr(kServerSectionPrefix.size());
    const auto url = section.find("url");
    if (url == section.end()) return absl::InvalidArgumentError(absl::StrCat("MCP server ", entry.name, " missing url"));
    entry.url = std::string(absl::StripAsciiWhitespace(url->second));
    if (const auto auth = section.find("auth"); auth != section.end()) entry.auth = std::string(absl::StripAsciiWhitespace(auth->second));
    if (const auto enabled = section.find("enabled"); enabled != section.end()) {
      auto enabled_or = ParseBool(absl::StripAsciiWhitespace(enabled->second));
      if (!enabled_or.ok()) return enabled_or.status();
      entry.enabled = *enabled_or;
    }
    if (const auto scopes = section.find("scopes"); scopes != section.end()) entry.scopes = ParseScopes(scopes->second);
    if (const auto token_path = section.find("token_path"); token_path != section.end()) entry.token_path = token_path->second;
    const absl::Status status = ValidateServerRegistryEntry(entry);
    if (!status.ok()) return status;
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  return entries;
}

absl::Status SaveServerRegistry(const std::string& path, const std::vector<ServerRegistryEntry>& entries) {
  for (const ServerRegistryEntry& entry : entries) {
    const absl::Status status = ValidateServerRegistryEntry(entry);
    if (!status.ok()) return status;
  }
  const std::filesystem::path registry_path(path);
  std::error_code error;
  if (!registry_path.parent_path().empty()) {
    std::filesystem::create_directories(registry_path.parent_path(), error);
    if (error) return absl::UnavailableError(absl::StrCat("Failed to create MCP registry directory: ", error.message()));
  }
  const std::filesystem::path temporary_path = registry_path.string() + ".tmp";
  std::ofstream file(temporary_path, std::ios::trunc);
  if (!file.is_open()) return absl::UnavailableError(absl::StrCat("Failed to write MCP registry: ", path));
  for (const ServerRegistryEntry& entry : entries) {
    file << "[server." << entry.name << "]\n";
    file << "url = " << entry.url << "\n";
    file << "auth = " << entry.auth << "\n";
    file << "enabled = " << (entry.enabled ? "true" : "false") << "\n";
    if (!entry.scopes.empty()) file << "scopes = " << absl::StrJoin(entry.scopes, " ") << "\n";
    if (!entry.token_path.empty()) file << "token_path = " << entry.token_path << "\n";
    file << "\n";
  }
  file.close();
  if (!file.good()) return absl::UnavailableError(absl::StrCat("Failed to write MCP registry: ", path));
  std::filesystem::rename(temporary_path, registry_path, error);
  if (error) {
    std::filesystem::remove(temporary_path, error);
    return absl::UnavailableError(absl::StrCat("Failed to replace MCP registry: ", error.message()));
  }
  return absl::OkStatus();
}

absl::Status UpsertServerRegistryEntry(const std::string& path, const ServerRegistryEntry& entry) {
  auto entries_or = LoadServerRegistry(path);
  if (!entries_or.ok()) return entries_or.status();
  bool replaced = false;
  for (ServerRegistryEntry& existing : *entries_or) {
    if (existing.name == entry.name) {
      existing = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) entries_or->push_back(entry);
  std::sort(entries_or->begin(), entries_or->end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  return SaveServerRegistry(path, *entries_or);
}

absl::Status RemoveServerRegistryEntry(const std::string& path, const std::string& name) {
  auto entries_or = LoadServerRegistry(path);
  if (!entries_or.ok()) return entries_or.status();
  const size_t old_size = entries_or->size();
  entries_or->erase(std::remove_if(entries_or->begin(), entries_or->end(), [&](const auto& entry) { return entry.name == name; }),
                    entries_or->end());
  if (entries_or->size() == old_size) return absl::NotFoundError(absl::StrCat("MCP server not found: ", name));
  return SaveServerRegistry(path, *entries_or);
}

}  // namespace slop::mcp
