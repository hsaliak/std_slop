#ifndef SENTINEL_COMPLETION_H_
#define SENTINEL_COMPLETION_H_

#include <string>

namespace sentinel {

// Initialize readline completions from a JSON file.
void InitCompletion(const std::string& config_path);

} // namespace sentinel

#endif // SENTINEL_COMPLETION_H_
