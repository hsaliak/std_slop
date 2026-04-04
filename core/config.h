#ifndef SLOP_CORE_CONFIG_H_
#define SLOP_CORE_CONFIG_H_

#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace slop {

struct LlmToolSpecializationConfig {
  std::string tool_name;
  std::string system_prompt_patch;
  std::string session_id;
  std::string skill;
  std::optional<int> context_window;
};

// Loads configuration from an INI file and applies it to absl flags.
// If override_path is empty, it looks for the default config at
// ~/.config/slop/config.ini.
// Settings in the INI file will NOT override flags already specified
// on the command line.
void LoadConfigAndApply(const std::string& override_path = "");

// Loads llm_query tool specializations from sections named [llm_tool_<name>]
// in the active INI config file.
// Returns an empty vector if no config file exists or there are no matching
// sections.
absl::StatusOr<std::vector<LlmToolSpecializationConfig>>
LoadLlmToolSpecializations(const std::string& override_path = "");

// Parses llm_query tool specializations from raw INI content.
absl::StatusOr<std::vector<LlmToolSpecializationConfig>>
LoadLlmToolSpecializationsFromIni(std::string_view ini_content);

}  // namespace slop

#endif  // SLOP_CORE_CONFIG_H_
