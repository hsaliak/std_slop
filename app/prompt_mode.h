#ifndef SLOP_APP_PROMPT_MODE_H_
#define SLOP_APP_PROMPT_MODE_H_

#include <istream>
#include <string>

#include "absl/status/status.h"
#include "nlohmann/json.hpp"
#include "absl/status/statusor.h"
#include "interface/interaction_engine.h"

namespace slop {

struct PromptInputFlags {
  std::string prompt;
  std::string prompt_file;
};

struct StructuredFormatFlags {
  std::string format;
  std::string format_file;
};

bool HasPromptInputSource(const PromptInputFlags& flags);

bool HasStructuredFormatSource(const StructuredFormatFlags& flags);

absl::StatusOr<nlohmann::json> ResolveStructuredOutputSchema(const StructuredFormatFlags& flags);

absl::Status ValidatePromptModePreflight(const PromptInputFlags& prompt_flags,
                                         const StructuredFormatFlags& format_flags,
                                         const std::string& output_mode);

absl::Status ValidatePromptOutputMode(const std::string& output_mode);

absl::StatusOr<std::string> ResolvePromptInput(const PromptInputFlags& flags, std::istream* context_stream);

std::string PromptRunResultToJson(const InteractionEngine::PromptRunResult& result);

}  // namespace slop

#endif  // SLOP_APP_PROMPT_MODE_H_
