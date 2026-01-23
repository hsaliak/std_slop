#include "completer.h"
#include "absl/strings/match.h"
#include <algorithm>

namespace slop {

std::vector<std::string> FilterCommands(const std::string& prefix, const std::vector<std::string>& commands) {
  if (prefix.empty()) return commands;
  
  std::vector<std::string> filtered;
  for (const auto& cmd : commands) {
    if (absl::StartsWith(cmd, prefix)) {
      filtered.push_back(cmd);
    }
  }
  std::sort(filtered.begin(), filtered.end());
  return filtered;
}

} // namespace slop
