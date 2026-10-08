#include "mcp/decision_api/server.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "mcp/types.h"

namespace slop::mcp::decision_api {
namespace {

std::string SanitizedEndpoint(const std::string& endpoint) {
  const std::size_t query = endpoint.find('?');
  return endpoint.substr(0, query);
}

nlohmann::json StructuredValueSchema() {
  return {{"oneOf", {{{"type", "string"}}, {{"type", "object"}}, {{"type", "array"}}}}};
}

nlohmann::json QuestionSchema(const std::string& type) {
  const nlohmann::json description = StructuredValueSchema();
  nlohmann::json properties = {{"type", {{"type", "string"}, {"enum", {type}}}}, {"instructions", description}};
  nlohmann::json required = {"type", "instructions"};
  if (type == "noul") {
    properties["criteria"] = {{"type", "object"},
                              {"properties", {{"true", description}, {"false", description}}},
                              {"required", {"true", "false"}},
                              {"additionalProperties", false}};
  } else if (type == "choice") {
    properties["criteria"] = {{"type", "object"}, {"additionalProperties", description}};
    required.push_back("criteria");
  } else {
    properties["criteria"] = {{"type", "array"}, {"minItems", 2}, {"maxItems", 64}, {"items", description}};
    required.push_back("criteria");
  }
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false}};
}

nlohmann::json DecideInputSchema() {
  const nlohmann::json trace_schema = {{"type", "object"},
                                       {"properties",
                                        {{"trace_id", {{"type", "string"}, {"maxLength", 256}}},
                                         {"trace_name", {{"type", "string"}, {"maxLength", 256}}},
                                         {"span_name", {{"type", "string"}, {"maxLength", 256}}}}},
                                       {"additionalProperties", false}};
  const nlohmann::json options_schema = {{"type", "object"},
                                         {"properties",
                                          {{"session_id", {{"type", "string"}, {"maxLength", 256}}},
                                           {"trace", trace_schema},
                                           {"user", {{"type", "string"}, {"maxLength", 256}}},
                                           {"provider", {{"type", "object"}}}}},
                                         {"additionalProperties", false}};
  return {{"type", "object"},
          {"properties",
           {{"state", StructuredValueSchema()},
            {"questions",
             {{"type", "object"},
              {"additionalProperties",
               {{"oneOf", {QuestionSchema("noul"), QuestionSchema("choice"), QuestionSchema("score")}}}}}},
            {"model", {{"type", "string"}, {"maxLength", 256}}},
            {"openrouterOptions", options_schema}}},
          {"required", {"state", "questions"}},
          {"additionalProperties", false}};
}

mcp::ToolCallResult ToolResult(std::string text, nlohmann::json structured, bool is_error = false) {
  mcp::ToolCallResult result;
  result.content.push_back({{"type", "text"}, {"text", std::move(text)}});
  result.structured_content = std::move(structured);
  result.is_error = is_error;
  return result;
}

mcp::server::ToolRegistration MakeTool(std::string name, std::string description, nlohmann::json schema,
                                       nlohmann::json annotations, mcp::server::ToolHandler handler) {
  mcp::server::ToolRegistration registration;
  registration.definition.name = std::move(name);
  registration.definition.description = std::move(description);
  registration.definition.input_schema = std::move(schema);
  registration.definition.annotations = std::move(annotations);
  registration.handler = std::move(handler);
  return registration;
}

absl::StatusOr<mcp::ToolCallResult> ErrorResult(const absl::Status& status) {
  return ToolResult(std::string(status.message()), {{"error", {{"message", std::string(status.message())}}}}, true);
}

std::string HelpText(const std::string& topic, const Config& config) {
  if (topic == "overview") {
    return absl::StrCat(
        "Decision API MCP uses OpenRouter. Evaluation endpoint: ", SanitizedEndpoint(config.endpoint),
        ". Configured model is passed through unchanged: ", config.model,
        ". Use decision_models to copy an exact catalog ID such as ~typesafe/jev-latest; aliases may include namespace "
        "prefixes and are never rewritten. Calls may incur charges and disclose state.");
  }
  if (topic == "noul")
    return "Noul: ask a yes/no question. The noul answer is P(yes), from 0 to 1; near 0.5 means uncertainty, not "
           "intensity. Optional criteria must contain exactly true and false descriptions. No confidence field is "
           "promised.";
  if (topic == "choice")
    return "Choice: select one named option from the criteria map (1 to 255 options in this server). Include a none "
           "option when no match is possible. probabilities and confidence are optional response fields.";
  if (topic == "score")
    return "Score: rate state against 2 to 64 ordered levels, lowest first. score is the probability-weighted level "
           "index and may be fractional. legend and probabilities are optional. The 64-level cap is local.";
  if (topic == "request")
    return "decide requires state (string, object, or array) and a non-empty questions object. Each question has type "
           "and instructions; Choice and Score also require criteria. At most 32 questions may be batched. Independent "
           "questions "
           "batched.";
  if (topic == "response")
    return "Answers are keyed by submitted question IDs. Every answer has a matching type and primitive value. "
           "Probabilities, confidence, and legend are optional where applicable. Missing/mismatched answers are "
           "errors; no default judgment is substituted.";
  if (topic == "configuration")
    return absl::StrCat(
        "The API key is held in the server's private config. The endpoint defaults to ", kDefaultEndpoint,
        ". timeoutMs defaults to 30000 (range 1-60000). A custom endpoint disables default catalog lookup.");
  if (topic == "models")
    return absl::StrCat("decision_models reads the OpenRouter catalog at ", kDefaultModelsEndpoint,
                        ". It returns catalog IDs as-is; aliases may differ from their resolved version IDs. If "
                        "discovery is disabled, no fallback host is contacted.");
  if (topic == "errors")
    return "Authentication, request validation, rate limit, overload, transport, timeout, and malformed response "
           "failures are reported as errors, never as low-confidence decisions. This server does not retry evaluation "
           "requests by default.";
  if (topic == "examples")
    return "Example: {\"state\":{\"ticket\":\"duplicate "
           "charge\"},\"questions\":{\"refund\":{\"type\":\"noul\",\"instructions\":\"Is a refund requested?\"}}}. "
           "Model aliases are passed unchanged; use decision_models to inspect current catalog IDs.";
  return {};
}

}  // namespace

absl::StatusOr<mcp::server::Server> CreateServer(std::shared_ptr<DecisionApiClient> client, const Config& config) {
  if (client == nullptr) return absl::InvalidArgumentError("decision API client must not be null");
  std::vector<mcp::server::ToolRegistration> tools;
  tools.push_back(
      MakeTool("decide",
               "Evaluate named noul, choice, and score questions against shared state using OpenRouter. "
               "Returns typed answers, resolved model, and available usage metadata. Noul is probability "
               "of yes; Choice selects a defined option; Score is a probability-weighted ordered-rubric "
               "index. This may incur provider charges and discloses input to an external service. It does "
               "not execute actions. Use decision_help for schemas, examples, and uncertainty guidance.",
               DecideInputSchema(),
               {{"readOnlyHint", true}, {"openWorldHint", true}, {"destructiveHint", false}, {"idempotentHint", false}},
               [client](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
                 auto response = client->Decide(arguments);
                 if (!response.ok()) return ErrorResult(response.status());
                 return ToolResult("OpenRouter decision completed", std::move(*response));
               }));

  const nlohmann::json help_schema = {{"type", "object"},
                                      {"properties",
                                       {{"topic",
                                         {{"type", "string"},
                                          {"enum",
                                           {"overview", "noul", "choice", "score", "request", "response",
                                            "configuration", "models", "errors", "examples"}}}}}},
                                      {"additionalProperties", false}};
  tools.push_back(
      MakeTool("decision_help",
               "Explain the OpenRouter Decision API schemas, question types, model aliases, configuration, errors, and "
               "uncertainty. This local documentation tool performs no HTTP request and reveals no credentials.",
               help_schema, {{"readOnlyHint", true}, {"openWorldHint", false}},
               [config](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
                 if (!arguments.is_object())
                   return ErrorResult(absl::InvalidArgumentError("help arguments must be an object"));
                 const auto topic_value = json_get<std::string>(arguments, "topic");
                 if (json_at(arguments, "topic") != nullptr && !topic_value.has_value()) {
                   return ErrorResult(absl::InvalidArgumentError("help topic must be a string"));
                 }
                 const std::string topic = topic_value.value_or("overview");
                 const std::string text = HelpText(topic, config);
                 if (text.empty()) return ErrorResult(absl::InvalidArgumentError("unknown help topic"));
                 return ToolResult(text, {{"topic", topic}, {"text", text}});
               }));

  const nlohmann::json empty_schema = {{"type", "object"}, {"additionalProperties", false}};
  tools.push_back(MakeTool(
      "decision_models",
      "List decision-capable OpenRouter models from its catalog, including exact IDs and available context/pricing "
      "metadata. Does not evaluate a request. Reports when discovery is disabled; never invents IDs or falls back to "
      "another host.",
      empty_schema, {{"readOnlyHint", true}, {"openWorldHint", true}},
      [client](const nlohmann::json& arguments) -> absl::StatusOr<mcp::ToolCallResult> {
        if (!arguments.is_object() || !arguments.empty())
          return ErrorResult(absl::InvalidArgumentError("decision_models expects an empty object"));
        auto models = client->Models();
        if (!models.ok()) return ErrorResult(models.status());
        const auto* data = json_at(*models, "data");
        return ToolResult(absl::StrCat("OpenRouter returned ", data == nullptr ? 0 : data->size(), " decision models"),
                          std::move(*models));
      }));

  mcp::ImplementationInfo identity;
  identity.name = "openrouter-decision-api";
  identity.version = "1.0.0";
  return mcp::server::Server::Create(std::move(identity), std::move(tools));
}

}  // namespace slop::mcp::decision_api
