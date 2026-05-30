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
  return !flags.prompt.empty() || !flags.prompt_file.empty();
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

absl::StatusOr<std::string> ResolvePromptInput(const PromptInputFlags& flags, std::istream* context_stream) {
  int source_count = 0;
  if (!flags.prompt.empty()) ++source_count;
  if (!flags.prompt_file.empty()) ++source_count;
  if (source_count != 1) {
    return absl::InvalidArgumentError("Specify exactly one instruction source: --prompt or --prompt-file.");
  }
  if (flags.prompt == "-") {
    return absl::InvalidArgumentError("--prompt=- is not supported. Use --prompt for the instruction; piped stdin is read as context.");
  }

  std::string instruction;
  if (!flags.prompt_file.empty()) {
    std::ifstream file(flags.prompt_file);
    if (!file.is_open()) {
      return absl::NotFoundError(absl::StrCat("Failed to open prompt file: ", flags.prompt_file));
    }
    auto instruction_or = ReadAll(&file);
    if (!instruction_or.ok()) return instruction_or.status();
    instruction = *instruction_or;
  } else {
    instruction = flags.prompt;
  }

  if (absl::StripAsciiWhitespace(instruction).empty()) {
    return absl::InvalidArgumentError("Prompt input must not be empty.");
  }

  if (context_stream == nullptr) {
    return instruction;
  }

  auto context_or = ReadAll(context_stream);
  if (!context_or.ok()) return context_or.status();
  if (absl::StripAsciiWhitespace(*context_or).empty()) {
    return instruction;
  }

  return absl::StrCat("Context:\n", *context_or, "\nInstruction:\n", instruction);
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
