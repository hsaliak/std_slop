#include "mcp/json_schema.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

#include "core/json_utils.h"

namespace slop::mcp {
namespace {

constexpr absl::string_view kDraft202012 = "https://json-schema.org/draft/2020-12/schema";

struct EvaluationContext {
  const nlohmann::json& root;
  JsonSchemaLimits limits;
  size_t evaluations = 0;
  absl::flat_hash_set<std::pair<const nlohmann::json*, const nlohmann::json*>> active;
};

absl::Status Consume(EvaluationContext* context, size_t depth) {
  if (context->limits.max_evaluations == 0 || ++context->evaluations > context->limits.max_evaluations) {
    return absl::ResourceExhaustedError("JSON Schema evaluation limit exceeded");
  }
  if (depth > context->limits.max_depth) {
    return absl::ResourceExhaustedError("JSON Schema nesting limit exceeded");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> DecodePointerToken(absl::string_view token) {
  std::string decoded;
  decoded.reserve(token.size());
  for (size_t i = 0; i < token.size(); ++i) {
    if (token[i] != '~') {
      decoded.push_back(token[i]);
      continue;
    }
    if (++i >= token.size()) {
      return absl::InvalidArgumentError("invalid local JSON Pointer escape");
    }
    if (token[i] == '0') {
      decoded.push_back('~');
    } else if (token[i] == '1') {
      decoded.push_back('/');
    } else {
      return absl::InvalidArgumentError("invalid local JSON Pointer escape");
    }
  }
  return decoded;
}

absl::StatusOr<const nlohmann::json*> ResolveLocalReference(const nlohmann::json& root, absl::string_view reference) {
  if (reference == "#") return &root;
  if (reference.size() < 2 || reference.substr(0, 2) != "#/") {
    return absl::UnimplementedError("external and non-pointer JSON Schema references are disabled");
  }
  const nlohmann::json* current = &root;
  size_t begin = 2;
  while (begin <= reference.size()) {
    const size_t slash = reference.find('/', begin);
    const absl::string_view encoded =
        reference.substr(begin, slash == absl::string_view::npos ? reference.size() - begin : slash - begin);
    auto token_or = DecodePointerToken(encoded);
    if (!token_or.ok()) return token_or.status();
    if (current->is_object()) {
      const auto it = current->find(*token_or);
      if (it == current->end()) {
        return absl::InvalidArgumentError(absl::StrCat("unresolved local JSON Schema reference: ", reference));
      }
      current = &*it;
    } else if (current->is_array()) {
      size_t index = 0;
      if (*token_or == "-" || !absl::SimpleAtoi(*token_or, &index) || index >= current->size()) {
        return absl::InvalidArgumentError(absl::StrCat("invalid JSON Schema array reference: ", reference));
      }
      current = &(*current)[index];
    } else {
      return absl::InvalidArgumentError(absl::StrCat("JSON Schema reference crosses a scalar: ", reference));
    }
    if (slash == absl::string_view::npos) break;
    begin = slash + 1;
  }
  return current;
}

bool MatchesType(absl::string_view type, const nlohmann::json& instance) {
  if (type == "null") return instance.is_null();
  if (type == "boolean") return instance.is_boolean();
  if (type == "object") return instance.is_object();
  if (type == "array") return instance.is_array();
  if (type == "number") return instance.is_number();
  if (type == "integer") {
    if (instance.is_number_integer() || instance.is_number_unsigned()) return true;
    return instance.is_number_float() && std::floor(instance.get<double>()) == instance.get<double>();
  }
  if (type == "string") return instance.is_string();
  return false;
}

absl::Status Validate(const nlohmann::json& schema, const nlohmann::json& instance, EvaluationContext* context,
                      size_t depth);

absl::Status ValidateSubschemas(const nlohmann::json& schemas, const nlohmann::json& instance,
                                EvaluationContext* context, size_t depth, absl::string_view keyword) {
  if (!schemas.is_array()) {
    return absl::InvalidArgumentError(absl::StrCat(keyword, " must be an array"));
  }
  size_t matches = 0;
  for (const auto& subschema : schemas) {
    const absl::Status status = Validate(subschema, instance, context, depth + 1);
    if (status.ok()) ++matches;
    if (keyword == "allOf" && !status.ok()) return status;
  }
  if (keyword == "anyOf" && matches == 0) {
    return absl::InvalidArgumentError("instance does not match anyOf");
  }
  if (keyword == "oneOf" && matches != 1) {
    return absl::InvalidArgumentError("instance must match exactly one oneOf branch");
  }
  return absl::OkStatus();
}

absl::Status Validate(const nlohmann::json& schema, const nlohmann::json& instance, EvaluationContext* context,
                      size_t depth) {
  absl::Status consumed = Consume(context, depth);
  if (!consumed.ok()) return consumed;
  if (schema.is_boolean()) {
    return schema.get<bool>() ? absl::OkStatus() : absl::InvalidArgumentError("false schema rejected instance");
  }
  if (!schema.is_object()) {
    return absl::InvalidArgumentError("JSON Schema must be an object or boolean");
  }

  const auto key = std::make_pair(&schema, &instance);
  if (!context->active.insert(key).second) return absl::OkStatus();
  struct ActiveGuard {
    EvaluationContext* context;
    std::pair<const nlohmann::json*, const nlohmann::json*> key;
    ~ActiveGuard() { context->active.erase(key); }
  } guard{context, key};

  if (const auto dialect = json_get<std::string>(schema, "$schema")) {
    if (*dialect != kDraft202012) {
      return absl::UnimplementedError(absl::StrCat("unsupported JSON Schema dialect: ", *dialect));
    }
  }
  if (const auto reference = json_get<std::string>(schema, "$ref")) {
    auto target_or = ResolveLocalReference(context->root, *reference);
    if (!target_or.ok()) return target_or.status();
    const absl::Status status = Validate(**target_or, instance, context, depth + 1);
    if (!status.ok()) return status;
  } else if (json_at(schema, "$ref") != nullptr) {
    return absl::InvalidArgumentError("JSON Schema $ref must be a string");
  }
  if (json_at(schema, "$dynamicRef") != nullptr || json_at(schema, "$dynamicAnchor") != nullptr) {
    return absl::UnimplementedError("dynamic JSON Schema references are not supported");
  }

  if (const auto* type = json_at(schema, "type")) {
    bool matches = false;
    if (type->is_string()) {
      matches = MatchesType(type->get_ref<const std::string&>(), instance);
    } else if (type->is_array()) {
      for (const auto& candidate : *type) {
        if (!candidate.is_string()) {
          return absl::InvalidArgumentError("JSON Schema type array must contain strings");
        }
        matches = matches || MatchesType(candidate.get_ref<const std::string&>(), instance);
      }
    } else {
      return absl::InvalidArgumentError("JSON Schema type must be a string or array");
    }
    if (!matches) return absl::InvalidArgumentError("instance has the wrong JSON type");
  }
  if (const auto* value = json_at(schema, "const"); value != nullptr && instance != *value) {
    return absl::InvalidArgumentError("instance does not match const");
  }
  if (const auto* values = json_at(schema, "enum")) {
    if (!values->is_array() || values->empty()) {
      return absl::InvalidArgumentError("JSON Schema enum must be a nonempty array");
    }
    bool found = false;
    for (const auto& value : *values) found = found || value == instance;
    if (!found) return absl::InvalidArgumentError("instance is not in enum");
  }

  for (absl::string_view keyword : {"allOf", "anyOf", "oneOf"}) {
    if (const auto* schemas = json_at(schema, std::string(keyword))) {
      const absl::Status status = ValidateSubschemas(*schemas, instance, context, depth, keyword);
      if (!status.ok()) return status;
    }
  }
  if (const auto* subschema = json_at(schema, "not")) {
    if (Validate(*subschema, instance, context, depth + 1).ok()) {
      return absl::InvalidArgumentError("instance matches prohibited not schema");
    }
  }

  if (instance.is_object()) {
    if (const auto* required = json_at(schema, "required")) {
      if (!required->is_array()) {
        return absl::InvalidArgumentError("required must be an array");
      }
      for (const auto& name : *required) {
        if (!name.is_string()) {
          return absl::InvalidArgumentError("required entries must be strings");
        }
        if (instance.find(name.get_ref<const std::string&>()) == instance.end()) {
          return absl::InvalidArgumentError(
              absl::StrCat("missing required property: ", name.get_ref<const std::string&>()));
        }
      }
    }
    const auto* properties = json_at(schema, "properties");
    if (properties != nullptr && !properties->is_object()) {
      return absl::InvalidArgumentError("properties must be an object");
    }
    for (auto it = instance.begin(); it != instance.end(); ++it) {
      const nlohmann::json* property_schema = properties == nullptr ? nullptr : json_at(*properties, it.key());
      if (property_schema != nullptr) {
        const absl::Status status = Validate(*property_schema, it.value(), context, depth + 1);
        if (!status.ok()) return status;
        continue;
      }
      if (const auto* additional = json_at(schema, "additionalProperties")) {
        if (additional->is_boolean() && !additional->get<bool>()) {
          return absl::InvalidArgumentError(absl::StrCat("additional property is not allowed: ", it.key()));
        }
        if (!additional->is_boolean()) {
          const absl::Status status = Validate(*additional, it.value(), context, depth + 1);
          if (!status.ok()) return status;
        }
      }
    }
  }

  if (instance.is_array()) {
    if (const auto maximum = json_get<size_t>(schema, "maxItems"); maximum && instance.size() > *maximum) {
      return absl::InvalidArgumentError("array exceeds maxItems");
    }
    if (const auto minimum = json_get<size_t>(schema, "minItems"); minimum && instance.size() < *minimum) {
      return absl::InvalidArgumentError("array is shorter than minItems");
    }
    size_t prefix_count = 0;
    if (const auto* prefix = json_at(schema, "prefixItems")) {
      if (!prefix->is_array()) {
        return absl::InvalidArgumentError("prefixItems must be an array");
      }
      prefix_count = prefix->size();
      for (size_t i = 0; i < instance.size() && i < prefix->size(); ++i) {
        const absl::Status status = Validate((*prefix)[i], instance[i], context, depth + 1);
        if (!status.ok()) return status;
      }
    }
    if (const auto* items = json_at(schema, "items")) {
      for (size_t i = prefix_count; i < instance.size(); ++i) {
        const absl::Status status = Validate(*items, instance[i], context, depth + 1);
        if (!status.ok()) return status;
      }
    }
  }

  if (instance.is_string()) {
    const size_t length = instance.get_ref<const std::string&>().size();
    if (const auto maximum = json_get<size_t>(schema, "maxLength"); maximum && length > *maximum) {
      return absl::InvalidArgumentError("string exceeds maxLength");
    }
    if (const auto minimum = json_get<size_t>(schema, "minLength"); minimum && length < *minimum) {
      return absl::InvalidArgumentError("string is shorter than minLength");
    }
    if (json_at(schema, "pattern") != nullptr) {
      return absl::UnimplementedError("JSON Schema pattern is not supported");
    }
  }

  if (instance.is_number()) {
    const double value = instance.get<double>();
    if (const auto minimum = json_get<double>(schema, "minimum"); minimum && value < *minimum) {
      return absl::InvalidArgumentError("number is less than minimum");
    }
    if (const auto maximum = json_get<double>(schema, "maximum"); maximum && value > *maximum) {
      return absl::InvalidArgumentError("number exceeds maximum");
    }
    if (const auto minimum = json_get<double>(schema, "exclusiveMinimum"); minimum && value <= *minimum) {
      return absl::InvalidArgumentError("number does not exceed exclusiveMinimum");
    }
    if (const auto maximum = json_get<double>(schema, "exclusiveMaximum"); maximum && value >= *maximum) {
      return absl::InvalidArgumentError("number does not precede exclusiveMaximum");
    }
  }
  return absl::OkStatus();
}

absl::Status Check(const nlohmann::json& schema, EvaluationContext* context, size_t depth,
                   absl::flat_hash_set<const nlohmann::json*>* active) {
  absl::Status consumed = Consume(context, depth);
  if (!consumed.ok()) return consumed;
  if (schema.is_boolean()) return absl::OkStatus();
  if (!schema.is_object()) {
    return absl::InvalidArgumentError("JSON Schema must be an object or boolean");
  }
  if (!active->insert(&schema).second) return absl::OkStatus();
  struct Guard {
    absl::flat_hash_set<const nlohmann::json*>* active;
    const nlohmann::json* schema;
    ~Guard() { active->erase(schema); }
  } guard{active, &schema};

  if (const auto dialect = json_get<std::string>(schema, "$schema")) {
    if (*dialect != kDraft202012) {
      return absl::UnimplementedError(absl::StrCat("unsupported JSON Schema dialect: ", *dialect));
    }
  }
  if (const auto reference = json_get<std::string>(schema, "$ref")) {
    auto target_or = ResolveLocalReference(context->root, *reference);
    if (!target_or.ok()) return target_or.status();
    const absl::Status status = Check(**target_or, context, depth + 1, active);
    if (!status.ok()) return status;
  } else if (json_at(schema, "$ref") != nullptr) {
    return absl::InvalidArgumentError("JSON Schema $ref must be a string");
  }
  if (json_at(schema, "$dynamicRef") != nullptr || json_at(schema, "$dynamicAnchor") != nullptr ||
      json_at(schema, "pattern") != nullptr) {
    return absl::UnimplementedError("JSON Schema contains an unsupported keyword");
  }
  for (absl::string_view keyword : {"allOf", "anyOf", "oneOf", "prefixItems"}) {
    if (const auto* children = json_at(schema, std::string(keyword))) {
      if (!children->is_array()) {
        return absl::InvalidArgumentError(absl::StrCat(keyword, " must be an array"));
      }
      for (const auto& child : *children) {
        const absl::Status status = Check(child, context, depth + 1, active);
        if (!status.ok()) return status;
      }
    }
  }
  for (absl::string_view keyword : {"not", "items", "additionalProperties"}) {
    if (const auto* child = json_at(schema, std::string(keyword))) {
      const absl::Status status = Check(*child, context, depth + 1, active);
      if (!status.ok()) return status;
    }
  }
  for (absl::string_view keyword : {"properties", "$defs"}) {
    if (const auto* children = json_at(schema, std::string(keyword))) {
      if (!children->is_object()) {
        return absl::InvalidArgumentError(absl::StrCat(keyword, " must be an object"));
      }
      for (const auto& [name, child] : children->items()) {
        (void)name;
        const absl::Status status = Check(child, context, depth + 1, active);
        if (!status.ok()) return status;
      }
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status ValidateJsonSchema(const nlohmann::json& schema, const nlohmann::json& instance, JsonSchemaLimits limits) {
  EvaluationContext context{schema, limits, 0, {}};
  absl::flat_hash_set<const nlohmann::json*> active;
  const absl::Status schema_status = Check(schema, &context, 0, &active);
  if (!schema_status.ok()) return schema_status;
  context.evaluations = 0;
  return Validate(schema, instance, &context, 0);
}

absl::Status CheckJsonSchema(const nlohmann::json& schema, JsonSchemaLimits limits) {
  EvaluationContext context{schema, limits, 0, {}};
  absl::flat_hash_set<const nlohmann::json*> active;
  return Check(schema, &context, 0, &active);
}

}  // namespace slop::mcp
