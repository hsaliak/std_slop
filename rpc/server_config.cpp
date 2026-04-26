#include "rpc/server_config.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "core/constants.h"
#include "core/status_macros.h"
#include "rpc/constants.h"
#include "google/protobuf/text_format.h"

namespace slop::rpc::v1 {
namespace {

absl::StatusOr<std::string> TrimNonEmpty(const std::string& value, const char* field_name) {
  std::string trimmed = std::string(absl::StripAsciiWhitespace(value));
  if (trimmed.empty()) {
    return absl::InvalidArgumentError(absl::StrCat(field_name, " is required"));
  }
  return trimmed;
}

std::string TrimOptional(const std::string& value) {
  return std::string(absl::StripAsciiWhitespace(value));
}

absl::Status ValidateNonNegative(int value, const char* field_name) {
  if (value < 0) {
    return absl::InvalidArgumentError(absl::StrCat(field_name, " must be non-negative"));
  }
  return absl::OkStatus();
}

absl::Status ValidateProvider(const ServerConfig& config) {
  if (!config.has_provider()) {
    return absl::InvalidArgumentError("provider is required");
  }

  const ProviderConfig& provider = config.provider();
  ASSIGN_OR_RETURN(const std::string provider_name, TrimNonEmpty(provider.provider(), "provider.provider"));
  if (provider_name != kProviderGemini && provider_name != kProviderOpenAi) {
    return absl::InvalidArgumentError("provider.provider must be 'gemini' or 'openai'");
  }
  ASSIGN_OR_RETURN(const std::string model, TrimNonEmpty(provider.model(), "provider.model"));

  if (provider.openai_oauth() && provider_name != kProviderOpenAi) {
    return absl::InvalidArgumentError("provider.openai_oauth requires provider.provider = 'openai'");
  }
  if (provider_name == kProviderGemini && provider.gemini_api_key_env().empty()) {
    return absl::InvalidArgumentError("provider.gemini_api_key_env is required for gemini provider");
  }
  if (provider_name == kProviderOpenAi && !provider.openai_oauth() && provider.openai_api_key_env().empty()) {
    return absl::InvalidArgumentError("provider.openai_api_key_env is required for openai provider without oauth");
  }

  return absl::OkStatus();
}

absl::Status ValidatePolicy(const ServerConfig& config) {
  if (!config.has_policy()) {
    return absl::InvalidArgumentError("policy is required");
  }
  const ServerPolicy& policy = config.policy();
  if (!policy.disable_ask_user()) {
    return absl::InvalidArgumentError("policy.disable_ask_user must be true for RPC server configs");
  }
  RETURN_IF_ERROR(ValidateNonNegative(policy.max_context_window(), "policy.max_context_window"));
  return absl::OkStatus();
}

absl::Status ValidateDefaults(const ServerConfig& config) {
  if (!config.has_defaults()) {
    return absl::OkStatus();
  }
  RETURN_IF_ERROR(ValidateNonNegative(config.defaults().context_window(), "defaults.context_window"));
  const int max_context_window = config.policy().max_context_window();
  if (max_context_window > 0 && config.defaults().context_window() > max_context_window) {
    return absl::InvalidArgumentError("defaults.context_window exceeds policy.max_context_window");
  }
  return absl::OkStatus();
}

absl::Status ValidateSpecializations(const ServerConfig& config) {
  for (const LlmToolSpecialization& specialization : config.llm_tool_specializations()) {
    ASSIGN_OR_RETURN(const std::string name, TrimNonEmpty(specialization.name(), "llm_tool_specializations.name"));
    ASSIGN_OR_RETURN(const std::string system_prompt_patch,
                     TrimNonEmpty(specialization.system_prompt_patch(), "llm_tool_specializations.system_prompt_patch"));
    ASSIGN_OR_RETURN(const std::string session_id,
                     TrimNonEmpty(specialization.session_id(), "llm_tool_specializations.session_id"));
    ASSIGN_OR_RETURN(const std::string skill, TrimNonEmpty(specialization.skill(), "llm_tool_specializations.skill"));
    RETURN_IF_ERROR(ValidateNonNegative(specialization.context_window(), "llm_tool_specializations.context_window"));
  }
  return absl::OkStatus();
}

std::string ReadEnv(const std::string& name) {
  if (name.empty()) {
    return "";
  }
  const char* value = std::getenv(name.c_str());
  return value == nullptr ? "" : std::string(value);
}

}  // namespace

ServerConfig ApplyServerConfigDefaults(const ServerConfig& config) {
  ServerConfig with_defaults = config;
  if (with_defaults.listen_addr().empty()) {
    with_defaults.set_listen_addr(kDefaultListenAddr);
  }
  if (with_defaults.db_path().empty()) {
    with_defaults.set_db_path(kDefaultDbPath);
  }
  return with_defaults;
}

absl::Status ValidateServerConfig(const ServerConfig& config) {
  const ServerConfig with_defaults = ApplyServerConfigDefaults(config);
  ASSIGN_OR_RETURN(const std::string listen_addr, TrimNonEmpty(with_defaults.listen_addr(), "listen_addr"));
  ASSIGN_OR_RETURN(const std::string db_path, TrimNonEmpty(with_defaults.db_path(), "db_path"));
  RETURN_IF_ERROR(ValidateProvider(with_defaults));
  RETURN_IF_ERROR(ValidatePolicy(with_defaults));
  RETURN_IF_ERROR(ValidateDefaults(with_defaults));
  RETURN_IF_ERROR(ValidateSpecializations(with_defaults));
  return absl::OkStatus();
}

absl::StatusOr<std::vector<slop::LlmToolSpecializationConfig>> ConvertLlmToolSpecializations(
    const ServerConfig& config) {
  std::vector<slop::LlmToolSpecializationConfig> specializations;
  specializations.reserve(config.llm_tool_specializations_size());
  for (const LlmToolSpecialization& specialization : config.llm_tool_specializations()) {
    slop::LlmToolSpecializationConfig converted;
    ASSIGN_OR_RETURN(const std::string name, TrimNonEmpty(specialization.name(), "llm_tool_specializations.name"));
    ASSIGN_OR_RETURN(const std::string system_prompt_patch,
                     TrimNonEmpty(specialization.system_prompt_patch(), "llm_tool_specializations.system_prompt_patch"));
    ASSIGN_OR_RETURN(const std::string session_id,
                     TrimNonEmpty(specialization.session_id(), "llm_tool_specializations.session_id"));
    ASSIGN_OR_RETURN(const std::string skill, TrimNonEmpty(specialization.skill(), "llm_tool_specializations.skill"));
    converted.tool_name = absl::StrCat("llm_tool_", name);
    converted.system_prompt_patch = system_prompt_patch;
    converted.session_id = session_id;
    converted.skill = skill;
    if (specialization.context_window() > 0) {
      converted.context_window = specialization.context_window();
    }
    specializations.push_back(std::move(converted));
  }
  return specializations;
}

absl::StatusOr<ServerRuntimeConfig> BuildServerRuntimeConfig(const ServerConfig& config) {
  const ServerConfig with_defaults = ApplyServerConfigDefaults(config);
  RETURN_IF_ERROR(ValidateServerConfig(with_defaults));

  ServerRuntimeConfig runtime_config;
  ASSIGN_OR_RETURN(runtime_config.listen_addr, TrimNonEmpty(with_defaults.listen_addr(), "listen_addr"));
  ASSIGN_OR_RETURN(runtime_config.db_path, TrimNonEmpty(with_defaults.db_path(), "db_path"));
  runtime_config.proto = with_defaults;
  runtime_config.active_skills.assign(with_defaults.defaults().active_skills().begin(),
                                      with_defaults.defaults().active_skills().end());
  if (with_defaults.defaults().context_window() > 0) {
    runtime_config.context_window = with_defaults.defaults().context_window();
  }

  const ServerPolicy& policy = with_defaults.policy();
  runtime_config.disable_ask_user = policy.disable_ask_user();
  runtime_config.allow_request_model_override = policy.allow_request_model_override();
  runtime_config.allow_request_skill_override = policy.allow_request_skill_override();
  runtime_config.allow_request_context_window_override = policy.allow_request_context_window_override();
  runtime_config.max_context_window = policy.max_context_window();

  const ProviderConfig& provider = with_defaults.provider();
  ASSIGN_OR_RETURN(runtime_config.runtime_options.model, TrimNonEmpty(provider.model(), "provider.model"));
  runtime_config.runtime_options.openai_oauth = provider.openai_oauth();
  runtime_config.runtime_options.use_responses = provider.openai_oauth() || provider.use_responses();
  runtime_config.runtime_options.openai_base_url = provider.openai_oauth()
                                                        ? kOpenAiChatGptCodexBaseUrl
                                                        : TrimOptional(provider.openai_base_url());
  runtime_config.runtime_options.google_api_key = ReadEnv(provider.gemini_api_key_env());
  runtime_config.runtime_options.openai_api_key = ReadEnv(provider.openai_api_key_env());
  ASSIGN_OR_RETURN(runtime_config.runtime_options.llm_specializations,
                   ConvertLlmToolSpecializations(with_defaults));

  return runtime_config;
}

absl::StatusOr<ServerConfig> LoadServerConfigTextproto(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return absl::NotFoundError(absl::StrCat("failed to open server config: ", path));
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  ServerConfig config;
  if (!google::protobuf::TextFormat::ParseFromString(content, &config)) {
    return absl::InvalidArgumentError(absl::StrCat("failed to parse server config textproto: ", path));
  }
  return config;
}

absl::StatusOr<ServerRuntimeConfig> LoadServerRuntimeConfig(const std::string& path) {
  ASSIGN_OR_RETURN(ServerConfig config, LoadServerConfigTextproto(path));
  return BuildServerRuntimeConfig(config);
}

}  // namespace slop::rpc::v1
