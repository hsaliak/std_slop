#include "app/prompt_mode.h"

#include <fstream>
#include <sstream>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "core/json_utils.h"
#include "nlohmann/json.hpp"

namespace slop {

bool HasPromptInputSource(const PromptInputFlags& flags) {
  return !flags.prompt.empty() || !flags.prompt_file.empty() || flags.prompt_stdin;
}

absl::Status ValidatePromptOutputMode(const std::string& output_mode) {
  if (output_mode == "text" || output_mode == "json") {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(absl::StrCat("Invalid --output value: ", output_mode, ". Expected text or json."));
}

namespace {

absl::StatusOr<std::string> ReadAll(std::istream* input) {
  if (input == nullptr) {
    return absl::InvalidArgumentError("Input stream is null");
  }
  std::ostringstream buffer;
  buffer << input->rdbuf();
  if (input->bad()) {
    return absl::InternalError("Failed to read prompt input");
  }
  return buffer.str();
}

}  // namespace

absl::StatusOr<std::string> ResolvePromptInput(const PromptInputFlags& flags, std::istream* stdin_stream) {
  const bool prompt_from_stdin_dash = flags.prompt == "-";
  int source_count = 0;
  if (!flags.prompt.empty()) ++source_count;
  if (!flags.prompt_file.empty()) ++source_count;
  if (flags.prompt_stdin) ++source_count;
  if (source_count != 1) {
    return absl::InvalidArgumentError("Specify exactly one prompt source: --prompt, --prompt-file, or --prompt-stdin.");
  }

  std::string prompt;
  if (!flags.prompt_file.empty()) {
    std::ifstream file(flags.prompt_file);
    if (!file.is_open()) {
      return absl::NotFoundError(absl::StrCat("Failed to open prompt file: ", flags.prompt_file));
    }
    auto prompt_or = ReadAll(&file);
    if (!prompt_or.ok()) return prompt_or.status();
    prompt = *prompt_or;
  } else if (flags.prompt_stdin || prompt_from_stdin_dash) {
    auto prompt_or = ReadAll(stdin_stream);
    if (!prompt_or.ok()) return prompt_or.status();
    prompt = *prompt_or;
  } else {
    prompt = flags.prompt;
  }

  if (absl::StripAsciiWhitespace(prompt).empty()) {
    return absl::InvalidArgumentError("Prompt input must not be empty.");
  }
  return prompt;
}

std::string PromptRunResultToJson(const InteractionEngine::PromptRunResult& result) {
  nlohmann::json output;
  output["ok"] = result.ok;
  output["session"] = result.session_id;
  output["model"] = result.model;
  output["active_skills"] = result.active_skills;
  output["assistant_message"] = result.assistant_message;
  if (result.ok) {
    output["error"] = nullptr;
  } else {
    output["error"] = nlohmann::json{{"code", result.error_code}, {"message", result.error_message}};
  }
  output["duration_ms"] = result.duration_ms;
  return json_dump(output);
}

}  // namespace slop
