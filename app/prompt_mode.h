#ifndef SLOP_APP_PROMPT_MODE_H_
#define SLOP_APP_PROMPT_MODE_H_

#include <istream>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "interface/interaction_engine.h"

namespace slop {

struct PromptInputFlags {
  std::string prompt;
  std::string prompt_file;
  bool prompt_stdin = false;
};

bool HasPromptInputSource(const PromptInputFlags& flags);

absl::Status ValidatePromptOutputMode(const std::string& output_mode);

absl::StatusOr<std::string> ResolvePromptInput(const PromptInputFlags& flags, std::istream* stdin_stream);

std::string PromptRunResultToJson(const InteractionEngine::PromptRunResult& result);

}  // namespace slop

#endif  // SLOP_APP_PROMPT_MODE_H_
