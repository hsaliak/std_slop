#include "core/config.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "absl/flags/flag.h"
#include "absl/flags/marshalling.h"
#include "absl/flags/reflection.h"
#include "absl/strings/str_cat.h"

#include "core/shell_util.h"
#include "ini/ini_parser.h"

namespace slop {

void LoadConfigAndApply(const std::string& override_path) {
  std::string config_path = override_path;
  if (config_path.empty()) {
    std::string home = GetHomeDir();
    if (!home.empty()) {
      config_path = absl::StrCat(home, "/.config/slop/config.ini");
    }
  }

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

}  // namespace slop
