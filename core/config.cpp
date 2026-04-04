#include "core/config.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "absl/flags/flag.h"
#include "absl/flags/marshalling.h"
#include "absl/flags/reflection.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/status_macros.h"
#include "core/shell_util.h"
#include "ini/ini_parser.h"

namespace slop {

namespace {

std::string ResolveConfigPath(const std::string& override_path) {
  std::string config_path = override_path;
  if (config_path.empty()) {
    std::string home = GetHomeDir();
    if (!home.empty()) {
      config_path = absl::StrCat(home, "/.config/slop/config.ini");
    }
  }
  return config_path;
}

absl::StatusOr<int> ParseNonNegativeInt(const std::string& raw_value, const std::string& key,
                                     const std::string& section_name) {
  int value = 0;
  auto [ptr, ec] = std::from_chars(raw_value.data(), raw_value.data() + raw_value.size(), value);
  if (ec != std::errc() || ptr != raw_value.data() + raw_value.size() || value < 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("Section [", section_name, "] has invalid ", key, ": '", raw_value, "'"));
  }
  return value;
}

}  // namespace

void LoadConfigAndApply(const std::string& override_path) {
  std::string config_path = ResolveConfigPath(override_path);

  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return;
  }

  std::ifstream f(config_path);
  if (!f.is_open()) {
    return;
  }

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  IniConfig config = ParseIni(content);

  auto it = config.find("slop");
  if (it == config.end()) {
    it = config.find("");  // Also support global section
  }

  if (it != config.end()) {
    for (const auto& [key, value] : it->second) {
      absl::CommandLineFlag* flag = absl::FindCommandLineFlag(key);
      if (flag && flag->CurrentValue() == flag->DefaultValue()) {
        std::string error;
        if (!flag->ParseFrom(value, &error)) {
          std::cerr << "Error parsing config key '" << key << "': " << error << std::endl;
        }
      }
    }
  }
}

absl::StatusOr<std::vector<LlmToolSpecializationConfig>> LoadLlmToolSpecializations(const std::string& override_path) {
  std::string config_path = ResolveConfigPath(override_path);
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return std::vector<LlmToolSpecializationConfig>{};
  }

  std::ifstream f(config_path);
  if (!f.is_open()) {
    return std::vector<LlmToolSpecializationConfig>{};
  }

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return LoadLlmToolSpecializationsFromIni(content);
}

absl::StatusOr<std::vector<LlmToolSpecializationConfig>> LoadLlmToolSpecializationsFromIni(
    const std::string_view ini_content) {
  std::string content(ini_content);
  IniConfig config = ParseIni(content);

  std::vector<LlmToolSpecializationConfig> specializations;
  for (const auto& entry : config) {
    const std::string& section_name = entry.first;
    const IniSection& section = entry.second;

    if (!absl::StartsWith(section_name, "llm_tool_")) continue;

    std::string tool_name = section_name.substr(std::string("llm_tool_").size());
    tool_name = std::string(absl::StripAsciiWhitespace(tool_name));
    if (tool_name.empty()) {
      return absl::InvalidArgumentError("Found [llm_tool_] section with empty tool name");
    }

    // Namespace config-defined tools to avoid collisions with built-ins.
    tool_name = absl::StrCat("llm_tool_", tool_name);

    auto require_key = [&](const char* key) -> absl::StatusOr<std::string> {
      auto it = section.find(key);
      if (it == section.end()) {
        return absl::InvalidArgumentError(absl::StrCat("Section [", section_name, "] missing required key: ", key));
      }
      std::string value = std::string(absl::StripAsciiWhitespace(it->second));
      if (value.empty()) {
        return absl::InvalidArgumentError(
            absl::StrCat("Section [", section_name, "] has empty required key: ", key));
      }
      return value;
    };

    LlmToolSpecializationConfig cfg;
    cfg.tool_name = tool_name;
    ASSIGN_OR_RETURN(cfg.system_prompt_patch, require_key("system_prompt_patch"));
    ASSIGN_OR_RETURN(cfg.session_id, require_key("session_id"));
    ASSIGN_OR_RETURN(cfg.skill, require_key("skill"));

    auto context_it = section.find("context_window");
    if (context_it != section.end()) {
      ASSIGN_OR_RETURN(cfg.context_window,
                       ParseNonNegativeInt(std::string(absl::StripAsciiWhitespace(context_it->second)),
                                        "context_window", section_name));
    }

    specializations.push_back(std::move(cfg));
  }

  return specializations;
}

}  // namespace slop
