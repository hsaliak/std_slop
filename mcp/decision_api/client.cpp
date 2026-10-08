#include "mcp/decision_api/client.h"

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

#include "core/json_utils.h"

namespace slop::mcp::decision_api {
namespace {

constexpr std::size_t kMaxQuestions = 32;
constexpr std::size_t kMaxRequestBytes = 1024 * 1024;
constexpr std::size_t kMaxResponseBytes = 4 * 1024 * 1024;
constexpr double kProbabilitySumTolerance = 0.02;

bool ContainsControl(const std::string& value) {
  for (unsigned char character : value) {
    if (character < 0x20 || character == 0x7f) return true;
  }
  return false;
}

bool IsWithinJsonDepth(const nlohmann::json& value, std::size_t depth = 0) {
  if (depth > 64) return false;
  if (value.is_array()) {
    for (const auto& child : value) {
      if (!IsWithinJsonDepth(child, depth + 1)) return false;
    }
  } else if (value.is_object()) {
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
      if (!IsWithinJsonDepth(iterator.value(), depth + 1)) return false;
    }
  }
  return true;
}

bool IsStructuredValue(const nlohmann::json& value) {
  return value.is_string() || value.is_object() || value.is_array();
}

bool IsFiniteProbability(const nlohmann::json& value) {
  if (!value.is_number()) return false;
  const auto number = json_getter<double>::get(value);
  if (!number.has_value()) return false;
  return std::isfinite(*number) && *number >= 0.0 && *number <= 1.0;
}

absl::Status ValidateQuestion(const nlohmann::json& question) {
  if (!question.is_object()) return absl::InvalidArgumentError("each question must be an object");
  const auto type = json_get<std::string>(question, "type");
  const auto instructions = json_at(question, "instructions");
  if (!type.has_value() || instructions == nullptr || !IsStructuredValue(*instructions)) {
    return absl::InvalidArgumentError("each question requires type and string/object/array instructions");
  }
  for (auto iterator = question.begin(); iterator != question.end(); ++iterator) {
    if (iterator.key() != "type" && iterator.key() != "instructions" && iterator.key() != "criteria") {
      return absl::InvalidArgumentError("question contains an unsupported field");
    }
  }
  const auto* criteria = json_at(question, "criteria");
  if (*type == "noul") {
    if (criteria == nullptr) return absl::OkStatus();
    if (!criteria->is_object() || criteria->size() != 2 || json_at(*criteria, "true") == nullptr ||
        json_at(*criteria, "false") == nullptr || !IsStructuredValue(*json_at(*criteria, "true")) ||
        !IsStructuredValue(*json_at(*criteria, "false"))) {
      return absl::InvalidArgumentError("noul criteria must contain string/object/array true and false descriptions");
    }
    return absl::OkStatus();
  }
  if (*type == "choice") {
    if (criteria == nullptr || !criteria->is_object() || criteria->empty() || criteria->size() > 255) {
      return absl::InvalidArgumentError("choice criteria must be an object with 1 to 255 options");
    }
    for (auto iterator = criteria->begin(); iterator != criteria->end(); ++iterator) {
      if (iterator.key().empty() || !IsStructuredValue(iterator.value())) {
        return absl::InvalidArgumentError(
            "choice options require non-empty names and string/object/array descriptions");
      }
    }
    return absl::OkStatus();
  }
  if (*type == "score") {
    if (criteria == nullptr || !criteria->is_array() || criteria->size() < 2 || criteria->size() > 64) {
      return absl::InvalidArgumentError("score criteria must contain 2 to 64 ordered levels");
    }
    for (const auto& criterion : *criteria) {
      if (!IsStructuredValue(criterion)) return absl::InvalidArgumentError("score levels must be string/object/array");
    }
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError("question type must be noul, choice, or score");
}

absl::Status ValidateAnswer(const nlohmann::json& question, const nlohmann::json& answer) {
  if (!answer.is_object() || json_get<std::string>(answer, "type") != json_get<std::string>(question, "type")) {
    return absl::InvalidArgumentError("answer type does not match submitted question");
  }
  const auto type = json_get<std::string>(question, "type");
  if (*type == "noul") {
    const auto value = json_at(answer, "noul");
    if (value == nullptr || !IsFiniteProbability(*value))
      return absl::InvalidArgumentError("noul answer must be a probability");
    return absl::OkStatus();
  }
  const auto* probabilities = json_at(answer, "probabilities");
  if (probabilities != nullptr) {
    if (!probabilities->is_object() || probabilities->empty()) {
      return absl::InvalidArgumentError("answer probabilities must be a non-empty object");
    }
    double total = 0;
    for (auto iterator = probabilities->begin(); iterator != probabilities->end(); ++iterator) {
      if (!IsFiniteProbability(iterator.value()))
        return absl::InvalidArgumentError("answer probability is out of range");
      total += *json_getter<double>::get(iterator.value());
    }
    const auto* criteria = json_at(question, "criteria");
    if (criteria == nullptr) return absl::InvalidArgumentError("choice/score probabilities require criteria");
    if (*type == "choice") {
      if (!criteria->is_object() || probabilities->size() != criteria->size()) {
        return absl::InvalidArgumentError("choice probabilities must cover every submitted option");
      }
      for (auto iterator = criteria->begin(); iterator != criteria->end(); ++iterator) {
        if (!probabilities->contains(iterator.key())) {
          return absl::InvalidArgumentError("choice probabilities must cover every submitted option");
        }
      }
    } else {
      if (!criteria->is_array() || probabilities->size() != criteria->size()) {
        return absl::InvalidArgumentError("score probabilities must cover every submitted level");
      }
      for (std::size_t index = 0; index < criteria->size(); ++index) {
        if (!probabilities->contains(absl::StrCat(index))) {
          return absl::InvalidArgumentError("score probabilities must cover every submitted level");
        }
      }
    }
    if (std::abs(total - 1.0) > kProbabilitySumTolerance) {
      return absl::InvalidArgumentError("answer probabilities must sum to 1");
    }
  }
  const auto* confidence = json_at(answer, "confidence");
  if (confidence != nullptr && !IsFiniteProbability(*confidence)) {
    return absl::InvalidArgumentError("answer confidence must be a probability");
  }
  if (*type == "score") {
    const auto* legend = json_at(answer, "legend");
    const auto* criteria = json_at(question, "criteria");
    if (legend != nullptr) {
      if (!legend->is_object() || criteria == nullptr || !criteria->is_array() || legend->size() != criteria->size()) {
        return absl::InvalidArgumentError("score legend must match the submitted rubric when present");
      }
      for (std::size_t index = 0; index < criteria->size(); ++index) {
        const std::string key = absl::StrCat(index);
        const auto* value = json_at(*legend, key);
        if (value == nullptr || *value != (*criteria)[index]) {
          return absl::InvalidArgumentError("score legend must preserve the submitted rubric");
        }
      }
    }
  }
  if (*type == "choice") {
    const auto choice = json_get<std::string>(answer, "choice");
    const auto* criteria = json_at(question, "criteria");
    if (!choice.has_value() || criteria == nullptr || !criteria->is_object() || !criteria->contains(*choice)) {
      return absl::InvalidArgumentError("choice answer must name one submitted option");
    }
    return absl::OkStatus();
  }
  const auto score = json_get<double>(answer, "score");
  const auto* criteria = json_at(question, "criteria");
  if (!score.has_value() || !std::isfinite(*score) || criteria == nullptr || !criteria->is_array() || *score < 0 ||
      *score > static_cast<double>(criteria->size() - 1)) {
    return absl::InvalidArgumentError("score answer is outside submitted rubric bounds");
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<nlohmann::json> DecisionApiClient::BuildRequest(const nlohmann::json& arguments, const Config& config) {
  if (!arguments.is_object()) return absl::InvalidArgumentError("decide arguments must be an object");
  for (auto iterator = arguments.begin(); iterator != arguments.end(); ++iterator) {
    if (iterator.key() != "state" && iterator.key() != "questions" && iterator.key() != "model" &&
        iterator.key() != "openrouterOptions") {
      return absl::InvalidArgumentError("decide arguments contain an unsupported field");
    }
  }
  if (!IsWithinJsonDepth(arguments)) return absl::InvalidArgumentError("decision arguments exceed 64 JSON levels");
  const auto* state = json_at(arguments, "state");
  const auto* questions = json_at(arguments, "questions");
  if (state == nullptr || !IsStructuredValue(*state) || questions == nullptr || !questions->is_object() ||
      questions->empty() || questions->size() > kMaxQuestions) {
    return absl::InvalidArgumentError("decide requires string/object/array state and 1 to 32 named questions");
  }
  const auto model = json_get<std::string>(arguments, "model");
  if (json_at(arguments, "model") != nullptr &&
      (!model.has_value() || model->empty() || model->size() > 256 || ContainsControl(*model))) {
    return absl::InvalidArgumentError("model override must be a non-empty model ID or alias of at most 256 bytes");
  }
  for (auto iterator = questions->begin(); iterator != questions->end(); ++iterator) {
    if (iterator.key().empty() || iterator.key().size() > 64 || ContainsControl(iterator.key())) {
      return absl::InvalidArgumentError("question IDs must be 1 to 64 bytes without control line breaks");
    }
    const absl::Status status = ValidateQuestion(iterator.value());
    if (!status.ok()) return status;
  }

  nlohmann::json request = {{"model", model.value_or(config.model)}, {"state", *state}, {"questions", *questions}};
  if (const auto* options = json_at(arguments, "openrouterOptions"); options != nullptr) {
    if (!options->is_object()) return absl::InvalidArgumentError("openrouterOptions must be an object");
    static const std::unordered_set<std::string> allowed_options = {"session_id", "trace", "user", "provider"};
    for (auto iterator = options->begin(); iterator != options->end(); ++iterator) {
      if (allowed_options.find(iterator.key()) == allowed_options.end()) {
        return absl::InvalidArgumentError("openrouterOptions contains an unsupported field");
      }
      if ((iterator.key() == "session_id" || iterator.key() == "user") &&
          (!json_getter<std::string>::is(iterator.value()) ||
           json_getter<std::string>::get(iterator.value())->size() > 256)) {
        return absl::InvalidArgumentError("session_id and user options must be strings of at most 256 bytes");
      }
      if (iterator.key() == "trace") {
        if (!iterator.value().is_object()) return absl::InvalidArgumentError("trace option must be an object");
        static const std::unordered_set<std::string> trace_fields = {"trace_id", "trace_name", "span_name"};
        for (auto field = iterator.value().begin(); field != iterator.value().end(); ++field) {
          const auto trace_value = json_get<std::string>(iterator.value(), field.key());
          if (trace_fields.find(field.key()) == trace_fields.end() || !trace_value.has_value() ||
              trace_value->size() > 256 || ContainsControl(*trace_value)) {
            return absl::InvalidArgumentError("trace contains an unsupported or invalid field");
          }
        }
      }
      if (iterator.key() == "provider" && !iterator.value().is_object()) {
        return absl::InvalidArgumentError("provider option must be an object");
      }
      request[iterator.key()] = iterator.value();
    }
  }
  if (json_dump(request).size() > kMaxRequestBytes)
    return absl::ResourceExhaustedError("decision request exceeds 1 MiB");
  return request;
}

absl::Status DecisionApiClient::ValidateResponse(const nlohmann::json& request, const nlohmann::json& response) {
  if (!response.is_object() || !json_get<std::string>(response, "model").has_value() || !json_at(response, "usage") ||
      !json_at(response, "usage")->is_object()) {
    return absl::InvalidArgumentError("Decision API response must contain model, answers, and usage");
  }
  const auto* questions = json_at(request, "questions");
  const auto* answers = json_at(response, "answers");
  if (questions == nullptr || answers == nullptr || !answers->is_object() || answers->size() != questions->size()) {
    return absl::InvalidArgumentError("Decision API response must contain one answer per question");
  }
  for (auto iterator = questions->begin(); iterator != questions->end(); ++iterator) {
    const auto* answer = json_at(*answers, iterator.key());
    if (answer == nullptr) return absl::InvalidArgumentError("Decision API response is missing a question answer");
    const absl::Status status = ValidateAnswer(iterator.value(), *answer);
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> DecisionApiClient::Decide(const nlohmann::json& arguments) const {
  if (http_client_ == nullptr) return absl::FailedPreconditionError("HTTP client is unavailable");
  auto request = BuildRequest(arguments, config_);
  if (!request.ok()) return request.status();
  const std::string body = json_dump(*request);
  const std::vector<std::string> headers = {absl::StrCat("Authorization: Bearer ", config_.api_key),
                                            "Content-Type: application/json", "Accept: application/json"};
  auto response = http_client_->PostOnceStreamWithResponse(
      config_.endpoint, body, headers, absl::Milliseconds(config_.timeout_ms), kMaxResponseBytes, nullptr);
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return absl::UnavailableError(absl::StrCat("OpenRouter Decisions API returned HTTP ", response->status_code));
  }
  auto value = json_parse(response->body);
  if (!value.has_value()) return absl::InvalidArgumentError("OpenRouter returned invalid JSON");
  const absl::Status status = ValidateResponse(*request, *value);
  if (!status.ok()) return status;
  return *value;
}

absl::StatusOr<nlohmann::json> DecisionApiClient::Models() const {
  if (http_client_ == nullptr) return absl::FailedPreconditionError("HTTP client is unavailable");
  if (!config_.models_endpoint.has_value())
    return absl::FailedPreconditionError("model discovery is disabled for this endpoint");
  const std::vector<std::string> headers = {absl::StrCat("Authorization: Bearer ", config_.api_key),
                                            "Accept: application/json"};
  auto response = http_client_->GetOnceWithResponse(*config_.models_endpoint, headers,
                                                    absl::Milliseconds(config_.timeout_ms), kMaxResponseBytes);
  if (!response.ok()) return response.status();
  if (response->status_code < 200 || response->status_code >= 300) {
    return absl::UnavailableError(absl::StrCat("OpenRouter model catalog returned HTTP ", response->status_code));
  }
  auto value = json_parse(response->body);
  if (!value.has_value() || !value->is_object())
    return absl::InvalidArgumentError("OpenRouter returned invalid model catalog JSON");
  const auto* models = json_at(*value, "data");
  if (models == nullptr || !models->is_array())
    return absl::InvalidArgumentError("OpenRouter model catalog lacks data array");
  for (const auto& model : *models) {
    if (!model.is_object() || !json_get<std::string>(model, "id").has_value()) {
      return absl::InvalidArgumentError("OpenRouter model catalog contains an invalid model entry");
    }
  }
  return *value;
}

}  // namespace slop::mcp::decision_api
