#ifndef SLOP_COMPLETER_H_
#define SLOP_COMPLETER_H_

#include <string>
#include <vector>
#include "absl/container/flat_hash_map.h"

namespace slop {

std::vector<std::string> FilterCommands(const std::string& prefix, const std::vector<std::string>& commands);

// New interface for setting completion commands
void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands);

}  // namespace slop

#endif  // SLOP_COMPLETER_H_
