#include "mcp/client/modern.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

#include "core/json_utils.h"
#include "mcp/json_rpc.h"
#include "mcp/json_schema.h"

namespace slop::mcp::v2026_07_28 {
namespace {

constexpr int64_t kMaxSafeInteger = 9007199254740991LL;

bool IsHttpToken(absl::string_view value) {
  if (value.empty()) return false;
  for (const unsigned char c : value) {
    if (absl::ascii_isalnum(c)) continue;
    switch (c) {
      case '!':
      case '#':
      case '$':
      case '%':
      case '&':
      case '\'':
      case '*':
      case '+':
      case '-':
      case '.':
      case '^':
      case '_':
      case '`':
      case '|':
      case '~':
        continue;
      default:
        return false;
    }
  }
  return true;
}

std::string EncodeHeaderValue(absl::string_view value) {
  bool safe = !value.empty() && !absl::ascii_isspace(value.front()) && !absl::ascii_isspace(value.back()) &&
              !(absl::StartsWith(value, "=?base64?") && absl::EndsWith(value, "?="));
  for (const unsigned char c : value) {
    safe = safe && c >= 0x20 && c <= 0x7e && c != 0x7f;
  }
  if (safe) return std::string(value);
  std::string encoded;
  absl::Base64Escape(value, &encoded);
  return absl::StrCat("=?base64?", encoded, "?=");
}

absl::StatusOr<ImplementationInfo> ParseImplementationInfo(const nlohmann::json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("implementation info must be an object");
  }
  ImplementationInfo info;
  info.name = json_get_or(value, "name", std::string{});
  info.version = json_get_or(value, "version", std::string{});
  info.title = json_get<std::string>(value, "title");
  if (info.name.empty() || info.version.empty()) {
    return absl::InvalidArgumentError("implementation info requires name and version");
  }
  return info;
}

absl::StatusOr<std::string> RequestName(const Request& request) {
  const char* field = nullptr;
  if (request.method == "tools/call" || request.method == "prompts/get") {
    field = "name";
  } else if (request.method == "resources/read") {
    field = "uri";
  } else {
    return std::string();
  }
  const auto name = json_get<std::string>(request.params, field);
  if (!name || name->empty()) {
    return absl::InvalidArgumentError(absl::StrCat(request.method, " requires params.", field));
  }
  return *name;
}

struct HeaderAnnotation {
  std::vector<std::string> path;
  std::string header;
  std::string type;
};

absl::Status CollectHeaderAnnotations(const nlohmann::json& schema, std::vector<std::string> path,
                                      absl::flat_hash_set<std::string>* names,
                                      std::vector<HeaderAnnotation>* annotations, size_t depth) {
  if (depth > 32) {
    return absl::ResourceExhaustedError("tool annotation nesting limit exceeded");
  }
  if (!schema.is_object()) {
    return absl::InvalidArgumentError("annotated tool schema must be an object");
  }
  if (json_at(schema, "$ref") != nullptr || json_at(schema, "allOf") != nullptr ||
      json_at(schema, "anyOf") != nullptr || json_at(schema, "oneOf") != nullptr || json_at(schema, "if") != nullptr) {
    if (json_at(schema, "x-mcp-header") != nullptr) {
      return absl::InvalidArgumentError("x-mcp-header cannot use references or composition");
    }
  }
  if (const auto header = json_get<std::string>(schema, "x-mcp-header")) {
    const std::string type = json_get_or(schema, "type", std::string{});
    if (!IsHttpToken(*header)) {
      return absl::InvalidArgumentError("x-mcp-header must be an HTTP token");
    }
    if (type != "string" && type != "integer" && type != "boolean") {
      return absl::InvalidArgumentError("x-mcp-header supports string, integer, or boolean parameters");
    }
    const std::string normalized = absl::AsciiStrToLower(*header);
    if (!names->insert(normalized).second) {
      return absl::InvalidArgumentError("x-mcp-header names must be unique case-insensitively");
    }
    annotations->push_back({std::move(path), *header, type});
  } else if (json_at(schema, "x-mcp-header") != nullptr) {
    return absl::InvalidArgumentError("x-mcp-header must be a string");
  }
  const auto* properties = json_at(schema, "properties");
  if (properties == nullptr) return absl::OkStatus();
  if (!properties->is_object()) {
    return absl::InvalidArgumentError("tool schema properties must be an object");
  }
  for (const auto& [name, child] : properties->items()) {
    std::vector<std::string> child_path = path;
    child_path.push_back(name);
    const absl::Status status = CollectHeaderAnnotations(child, std::move(child_path), names, annotations, depth + 1);
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}

const nlohmann::json* FindPath(const nlohmann::json& value, const std::vector<std::string>& path) {
  const nlohmann::json* current = &value;
  for (const std::string& component : path) {
    if (!current->is_object()) return nullptr;
    current = json_at(*current, component);
    if (current == nullptr) return nullptr;
  }
  return current;
}

absl::StatusOr<std::string> AnnotationValue(const HeaderAnnotation& annotation, const nlohmann::json& value) {
  if (annotation.type == "string" && value.is_string()) {
    return value.get_ref<const std::string&>();
  }
  if (annotation.type == "boolean" && value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (annotation.type == "integer" && (value.is_number_integer() || value.is_number_unsigned())) {
    if (value.is_number_unsigned()) {
      const uint64_t integer = value.get<uint64_t>();
      if (integer > static_cast<uint64_t>(kMaxSafeInteger)) {
        return absl::InvalidArgumentError("annotated integer exceeds the interoperable range");
      }
      return absl::StrCat(integer);
    }
    const int64_t integer = value.get<int64_t>();
    if (integer < -kMaxSafeInteger || integer > kMaxSafeInteger) {
      return absl::InvalidArgumentError("annotated integer exceeds the interoperable range");
    }
    return absl::StrCat(integer);
  }
  return absl::InvalidArgumentError("annotated argument does not match its declared type");
}

absl::StatusOr<Tool> ParseTool(const nlohmann::json& value) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError("tool entry must be an object");
  }
  Tool tool;
  tool.name = json_get_or(value, "name", std::string{});
  if (tool.name.empty()) return absl::InvalidArgumentError("tool missing name");
  tool.title = json_get<std::string>(value, "title");
  tool.description = json_get<std::string>(value, "description");
  const auto* input_schema = json_at(value, "inputSchema");
  if (input_schema == nullptr) {
    return absl::InvalidArgumentError("tool missing inputSchema");
  }
  tool.input_schema = *input_schema;
  absl::Status schema_status = CheckJsonSchema(tool.input_schema);
  if (!schema_status.ok()) return schema_status;
  if (const auto* output = json_at(value, "outputSchema")) {
    tool.output_schema = *output;
    schema_status = CheckJsonSchema(tool.output_schema);
    if (!schema_status.ok()) return schema_status;
  }
  if (const auto* annotations = json_at(value, "annotations")) {
    if (!annotations->is_object()) {
      return absl::InvalidArgumentError("tool annotations must be an object");
    }
    tool.annotations = *annotations;
  }
  if (const auto* meta = json_at(value, "_meta")) {
    if (!meta->is_object()) {
      return absl::InvalidArgumentError("tool _meta must be an object");
    }
    tool.meta = *meta;
  }
  absl::flat_hash_set<std::string> names;
  std::vector<HeaderAnnotation> header_annotations;
  const absl::Status annotation_status =
      CollectHeaderAnnotations(tool.input_schema, {}, &names, &header_annotations, 0);
  if (!annotation_status.ok()) return annotation_status;
  return tool;
}

}  // namespace

absl::StatusOr<EncodedRequest> EncodeRequest(const Request& request) {
  if (std::holds_alternative<std::monostate>(request.id)) {
    return absl::InvalidArgumentError("modern MCP requests require an id");
  }
  if (request.method.empty()) {
    return absl::InvalidArgumentError("modern MCP request method is empty");
  }
  if (!request.params.is_object()) {
    return absl::InvalidArgumentError("modern MCP params must be an object");
  }
  if (!request.context.client_capabilities.is_object()) {
    return absl::InvalidArgumentError("client capabilities must be an object");
  }
  if (request.context.client_info.name.empty() || request.context.client_info.version.empty()) {
    return absl::InvalidArgumentError("client info requires name and version");
  }

  nlohmann::json params = request.params;
  nlohmann::json meta = nlohmann::json::object();
  if (const auto* existing = json_at(params, "_meta")) {
    if (!existing->is_object()) {
      return absl::InvalidArgumentError("request _meta must be an object");
    }
    meta = *existing;
  }
  meta[kProtocolVersionMetadata] = std::string(kModernProtocolVersion);
  meta[kClientCapabilitiesMetadata] = request.context.client_capabilities;
  meta[kClientInfoMetadata] = {
      {"name", request.context.client_info.name},
      {"version", request.context.client_info.version},
  };
  if (request.context.client_info.title) {
    meta[kClientInfoMetadata]["title"] = *request.context.client_info.title;
  }
  params["_meta"] = std::move(meta);

  EncodedRequest encoded;
  encoded.body = BuildJsonRpcRequest(request.id, request.method, params);
  encoded.headers = {
      "Content-Type: application/json",
      "Accept: application/json, text/event-stream",
      absl::StrCat(kProtocolVersionHeader, ": ", kModernProtocolVersion),
      absl::StrCat("Mcp-Method: ", request.method),
  };
  auto name_or = RequestName(request);
  if (!name_or.ok()) return name_or.status();
  if (!name_or->empty()) {
    encoded.headers.push_back(absl::StrCat("Mcp-Name: ", EncodeHeaderValue(*name_or)));
  }

  if (request.tool_schema) {
    if (request.method != "tools/call") {
      return absl::InvalidArgumentError("tool schema is valid only for tools/call");
    }
    const auto* arguments = json_at(request.params, "arguments");
    if (arguments == nullptr || !arguments->is_object()) {
      return absl::InvalidArgumentError("tools/call requires object arguments");
    }
    absl::Status schema_status = ValidateJsonSchema(*request.tool_schema, *arguments);
    if (!schema_status.ok()) return schema_status;
    absl::flat_hash_set<std::string> names;
    std::vector<HeaderAnnotation> annotations;
    schema_status = CollectHeaderAnnotations(*request.tool_schema, {}, &names, &annotations, 0);
    if (!schema_status.ok()) return schema_status;
    for (const HeaderAnnotation& annotation : annotations) {
      const nlohmann::json* value = FindPath(*arguments, annotation.path);
      if (value == nullptr || value->is_null()) continue;
      auto value_or = AnnotationValue(annotation, *value);
      if (!value_or.ok()) return value_or.status();
      encoded.headers.push_back(absl::StrCat("Mcp-Param-", annotation.header, ": ", EncodeHeaderValue(*value_or)));
    }
  }
  return encoded;
}

absl::StatusOr<Discovery> ParseDiscovery(const nlohmann::json& result) {
  if (!result.is_object()) {
    return absl::InvalidArgumentError("server/discover result must be an object");
  }
  const auto versions = json_get<nlohmann::json::array_t>(result, "supportedVersions");
  if (!versions || versions->empty()) {
    return absl::InvalidArgumentError("server/discover requires supportedVersions");
  }
  Discovery discovery;
  for (const auto& version : *versions) {
    if (!version.is_string()) {
      return absl::InvalidArgumentError("supportedVersions entries must be strings");
    }
    discovery.supported_versions.push_back(version.get_ref<const std::string&>());
  }
  const auto* capabilities = json_at(result, "capabilities");
  if (capabilities == nullptr || !capabilities->is_object()) {
    return absl::InvalidArgumentError("server/discover requires object capabilities");
  }
  discovery.capabilities = *capabilities;
  discovery.instructions = json_get<std::string>(result, "instructions");
  if (const auto* meta = json_at(result, "_meta")) {
    if (!meta->is_object()) {
      return absl::InvalidArgumentError("discovery _meta must be an object");
    }
    if (const auto* info = json_at(*meta, kServerInfoMetadata)) {
      auto info_or = ParseImplementationInfo(*info);
      if (!info_or.ok()) return info_or.status();
      discovery.server_info = std::move(*info_or);
    }
  }
  return discovery;
}

absl::StatusOr<Result> ParseResult(const nlohmann::json& result) {
  if (!result.is_object()) {
    return absl::InvalidArgumentError("modern MCP result must be an object");
  }
  Result parsed;
  parsed.value = result;
  const auto* type_value = json_at(result, "resultType");
  if (type_value != nullptr && !type_value->is_string()) {
    return absl::InvalidArgumentError("modern MCP resultType must be a string");
  }
  const std::string type = json_get_or(result, "resultType", std::string{});
  if (type.empty() || type == "complete") {
    parsed.type = ResultType::kComplete;
  } else if (type == "input_required") {
    parsed.type = ResultType::kInputRequired;
    const auto* state = json_at(result, "requestState");
    if (state == nullptr) {
      return absl::InvalidArgumentError("input_required result missing requestState");
    }
    parsed.request_state = *state;
  } else {
    return absl::InvalidArgumentError(absl::StrCat("unknown modern MCP resultType: ", type));
  }
  return parsed;
}

absl::StatusOr<std::vector<Tool>> ParseToolsList(const nlohmann::json& result) {
  auto parsed_or = ParseResult(result);
  if (!parsed_or.ok()) return parsed_or.status();
  if (parsed_or->type != ResultType::kComplete) {
    return absl::FailedPreconditionError("tools/list cannot complete while input is required");
  }
  const auto* next_cursor = json_at(parsed_or->value, "nextCursor");
  if (next_cursor != nullptr && !next_cursor->is_string()) {
    return absl::InvalidArgumentError("tools/list nextCursor must be a string");
  }
  const auto tools = json_get<nlohmann::json::array_t>(parsed_or->value, "tools");
  if (!tools) {
    return absl::InvalidArgumentError("tools/list result missing tools array");
  }
  std::vector<Tool> parsed;
  parsed.reserve(tools->size());
  for (const auto& value : *tools) {
    auto tool_or = ParseTool(value);
    if (!tool_or.ok()) return tool_or.status();
    parsed.push_back(std::move(*tool_or));
  }
  return parsed;
}

absl::Status ValidateToolArguments(const Tool& tool, const nlohmann::json& arguments) {
  if (!arguments.is_object()) {
    return absl::InvalidArgumentError("tool arguments must be an object");
  }
  return ValidateJsonSchema(tool.input_schema, arguments);
}

absl::StatusOr<ToolCallResult> ParseToolCallResult(const nlohmann::json& result, const nlohmann::json* output_schema) {
  auto parsed_or = ParseResult(result);
  if (!parsed_or.ok()) return parsed_or.status();
  ToolCallResult parsed;
  parsed.kind = parsed_or->type == ResultType::kComplete ? ToolResultKind::kComplete : ToolResultKind::kInputRequired;
  parsed.request_state = parsed_or->request_state;
  const auto content = json_get<nlohmann::json::array_t>(parsed_or->value, "content");
  if (!content) {
    return absl::InvalidArgumentError("tools/call result missing content array");
  }
  for (const auto& block : *content) {
    if (!block.is_object() || !json_get<std::string>(block, "type").has_value()) {
      return absl::InvalidArgumentError("tool result content entries must be typed objects");
    }
    parsed.content.push_back(block);
  }
  parsed.is_error = json_get_or(parsed_or->value, "isError", false);
  if (const auto* structured = json_at(parsed_or->value, "structuredContent")) {
    if (output_schema != nullptr) {
      const absl::Status status = ValidateJsonSchema(*output_schema, *structured);
      if (!status.ok()) return status;
    }
    parsed.structured_content = *structured;
  }
  if (const auto* meta = json_at(parsed_or->value, "_meta")) {
    if (!meta->is_object()) {
      return absl::InvalidArgumentError("tool result _meta must be an object");
    }
    parsed.meta = *meta;
  }
  return parsed;
}

}  // namespace slop::mcp::v2026_07_28
