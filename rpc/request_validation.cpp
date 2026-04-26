#include "rpc/request_validation.h"

#include <string>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"

namespace slop::rpc::v1 {
namespace {

std::string Trim(const std::string& value) {
  return std::string(absl::StripAsciiWhitespace(value));
}

}  // namespace

absl::StatusOr<ValidatedRunPromptRequest> ValidateRunPromptRequest(const RunPromptRequest& request,
                                                                    const ServerRuntimeConfig& server_config) {
  const std::string prompt = Trim(request.prompt());
  if (prompt.empty()) {
    return absl::InvalidArgumentError("prompt is required");
  }

  if (!request.model_override().empty() && !server_config.allow_request_model_override) {
    return absl::InvalidArgumentError("model_override is not allowed by server policy");
  }
  if (!request.active_skills().empty() && !server_config.allow_request_skill_override) {
    return absl::InvalidArgumentError("active_skills override is not allowed by server policy");
  }
  if (request.has_context_window() && !server_config.allow_request_context_window_override) {
    return absl::InvalidArgumentError("context_window override is not allowed by server policy");
  }
  if (request.has_context_window() && request.context_window() < 0) {
    return absl::InvalidArgumentError("context_window must be non-negative");
  }
  if (request.has_context_window() && server_config.max_context_window > 0 &&
      request.context_window() > server_config.max_context_window) {
    return absl::InvalidArgumentError(absl::StrCat("context_window exceeds max_context_window ",
                                                  server_config.max_context_window));
  }

  ValidatedRunPromptRequest validated;
  validated.prompt = prompt;
  validated.session_id = Trim(request.session_id());
  if (!request.model_override().empty()) {
    validated.model_override = Trim(request.model_override());
  }
  if (request.active_skills().empty()) {
    validated.active_skills_override = false;
    validated.active_skills = server_config.active_skills;
  } else {
    validated.active_skills_override = true;
    validated.active_skills.assign(request.active_skills().begin(), request.active_skills().end());
  }
  if (request.has_context_window()) {
    validated.context_window = request.context_window();
  } else {
    validated.context_window = server_config.context_window;
  }
  return validated;
}

}  // namespace slop::rpc::v1
