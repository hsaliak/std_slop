#ifndef SLOP_CORE_CONFIG_H_
#define SLOP_CORE_CONFIG_H_

#include <string>

namespace slop {

// Loads configuration from an INI file and applies it to absl flags.
// If override_path is empty, it looks for the default config at
// ~/.config/slop/config.ini.
// Settings in the INI file will NOT override flags already specified
// on the command line.
void LoadConfigAndApply(const std::string& override_path = "");

}  // namespace slop

#endif  // SLOP_CORE_CONFIG_H_
