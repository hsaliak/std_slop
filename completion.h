#ifndef SLOP_COMPLETION_H_
#define SLOP_COMPLETION_H_

#include <string>

namespace slop {

// Initialize readline completions from a JSON file.
void InitCompletion(const std::string& config_path);

} // namespace slop

#endif // SLOP_COMPLETION_H_
