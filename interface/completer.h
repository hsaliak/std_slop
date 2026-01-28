#ifndef SLOP_COMPLETER_H_
#define SLOP_COMPLETER_H_

#include <string>
#include <vector>

namespace slop {

std::vector<std::string> FilterCommands(const std::string& prefix, const std::vector<std::string>& commands);

}  // namespace slop

#endif  // SLOP_COMPLETER_H_
