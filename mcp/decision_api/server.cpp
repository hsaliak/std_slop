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
                                        {{"generation_name", {{"type", "string"}, {"maxLength", 256}}},
                                         {"parent_span_id", {{"type", "string"}, {"maxLength", 256}}},
                                         {"trace_id", {{"type", "string"}, {"maxLength", 256}}},
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
           "should share state; dependent questions need a follow-up call.";
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
    return "OpenRouter documents HTTP 400, 401, 402, 403, 404, 413, 429, 500, 502, 503, 524, and 529 responses. "
           "Errors are reported as errors, never as low-confidence decisions. This server does not retry evaluation "
           "requests by default.";
  if (topic == "examples")
    return "Use the structured request/response examples returned with this help result. Model aliases are passed "
           "unchanged; use decision_models to inspect current catalog IDs.";
  return {};
}

nlohmann::json DecisionResponseSchema() {
  const nlohmann::json probability = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  const nlohmann::json probability_map = {{"type", "object"}, {"additionalProperties", probability}};

  nlohmann::json noul_answer = nlohmann::json::object();
  noul_answer["type"] = "object";
  noul_answer["properties"] = {{"type", {{"const", "noul"}}}, {"noul", probability}};
  noul_answer["required"] = {"type", "noul"};

  nlohmann::json choice_answer = nlohmann::json::object();
  choice_answer["type"] = "object";
  choice_answer["properties"] = {{"type", {{"const", "choice"}}},
                                 {"choice", {{"type", "string"}}},
                                 {"confidence", probability},
                                 {"probabilities", probability_map}};
  choice_answer["required"] = {"type", "choice"};

  nlohmann::json score_answer = nlohmann::json::object();
  score_answer["type"] = "object";
  score_answer["properties"] = {{"type", {{"const", "score"}}},
                                {"score", {{"type", "number"}}},
                                {"confidence", probability},
                                {"legend", {{"type", "object"}, {"additionalProperties", StructuredValueSchema()}}},
                                {"probabilities", probability_map}};
  score_answer["required"] = {"type", "score"};

  nlohmann::json answers = {{"type", "object"}};
  answers["additionalProperties"] = {{"oneOf", {noul_answer, choice_answer, score_answer}}};
  const nlohmann::json usage = {{"type", "object"},
                                {"properties",
                                 {{"input_tokens", {{"type", "integer"}}},
                                  {"output_tokens", {{"type", "integer"}}},
                                  {"cost", {{"type", "number"}}}}},
                                {"required", {"input_tokens", "output_tokens"}}};
  nlohmann::json properties = nlohmann::json::object();
  properties["model"] = {{"type", "string"}};
  properties["answers"] = answers;
  properties["usage"] = usage;
  properties["id"] = {{"type", "string"}};
  properties["provider"] = {{"type", "string"}};
  return {{"type", "object"}, {"properties", properties}, {"required", {"model", "answers", "usage"}}};
}

nlohmann::json DecisionHelpExamples() {
  nlohmann::json request = nlohmann::json::object();
  request["model"] = "~typesafe/jev-latest";
  request["state"] = {{"ticket", "A duplicate charge blocked checkout."}};
  request["questions"]["refund"] = {
      {"type", "noul"},
      {"instructions", "Is the customer asking for money back?"},
      {"criteria",
       {{"true", "They ask to reverse a duplicate charge."}, {"false", "They do not ask for money to be returned."}}}};
  request["questions"]["team"] = {{"type", "choice"},
                                  {"instructions", "Which team should handle the ticket?"},
                                  {"criteria",
                                   {{"billing", "Charges and refunds."},
                                    {"technical", "Checkout failures."},
                                    {"none", "Neither team is appropriate."}}}};
  request["questions"]["urgency"] = {{"type", "score"},
                                     {"instructions", "How urgent is the issue?"},
                                     {"criteria", {"Can wait", "Needs attention soon", "Blocks purchases now"}}};

  nlohmann::json response = nlohmann::json::object();
  response["model"] = "~typesafe/jev-latest";
  response["answers"]["refund"] = {{"type", "noul"}, {"noul", 0.97}};
  response["answers"]["team"] = {{"type", "choice"},
                                 {"choice", "billing"},
                                 {"probabilities", {{"billing", 0.94}, {"technical", 0.04}, {"none", 0.02}}}};
  response["answers"]["urgency"] = {
      {"type", "score"},
      {"score", 1.9},
      {"legend", {{"0", "Can wait"}, {"1", "Needs attention soon"}, {"2", "Blocks purchases now"}}},
      {"probabilities", {{"0", 0.02}, {"1", 0.06}, {"2", 0.92}}}};
  response["usage"] = {{"input_tokens", 23}, {"output_tokens", 8}, {"cost", 0.00001}};
  return {{"request", std::move(request)}, {"response", std::move(response)}};
}

nlohmann::json DecisionHelpContent(const std::string& topic, const std::string& text) {
  return {{"topic", topic},
          {"text", text},
          {"schemas", {{"decide", DecideInputSchema()}, {"response", DecisionResponseSchema()}}},
          {"examples", DecisionHelpExamples()}};
}

}  // namespace

absl::StatusOr<mcp::server::Server> CreateServer(std::shared_ptr<DecisionApiClient> client, const Config& config) {
  if (client == nullptr) return absl::InvalidArgumentError("decision API client must not be null");
  std::vector<mcp::server::ToolRegistration> tools;
  tools.push_back(MakeTool(
      "decide",
      "Evaluate named noul, choice, and score questions against shared state using OpenRouter. "
      "Returns typed answers, resolved model, and available usage metadata. Noul is probability "
      "of yes; Choice selects a defined option; Score is a probability-weighted ordered-rubric "
      "index. This may incur provider charges and discloses input to an external service. It does "
      "not execute actions. openrouterOptions accepts session_id, trace (generation_name, parent_span_id, "
      "span_name, trace_id, trace_name), user, and provider routing preferences. Use decision_help for schemas, "
      "examples, and uncertainty guidance.",
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
                 return ToolResult(text, DecisionHelpContent(topic, text));
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
