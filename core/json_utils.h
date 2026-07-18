#ifndef SLOP_CORE_JSON_UTILS_H_
#define SLOP_CORE_JSON_UTILS_H_

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"

namespace slop {

// Safely parse a JSON string without exceptions.
inline std::optional<nlohmann::json> json_parse(std::string_view s) {
  auto j = nlohmann::json::parse(s, nullptr, false);
  if (j.is_discarded()) {
    return std::nullopt;
  }
  return j;
}

// Forward declarations
template <typename T>
struct json_getter;

template <typename T>
inline std::optional<T> json_get(const nlohmann::json& j, const std::string& key);

// Base template for getting values from a JSON object
template <typename T>
struct json_getter {
  static bool is(const nlohmann::json& j) {
    if (j.is_null()) return false;
    if constexpr (std::is_same_v<T, std::string>) return j.is_string();
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) return j.is_number_integer();
    if constexpr (std::is_floating_point_v<T>) return j.is_number();
    if constexpr (std::is_same_v<T, bool>) return j.is_boolean();
    if constexpr (std::is_same_v<T, nlohmann::json>) return true;
    return false;
  }

  static std::optional<T> get(const nlohmann::json& j) {
    if (!is(j)) return std::nullopt;
    if constexpr (std::is_same_v<T, nlohmann::json>)
      return j;
    else
      return j.get<T>();
  }
};

// Specialization for std::optional
template <typename T>
struct json_getter<std::optional<T>> {
  static bool is(const nlohmann::json& j) { return j.is_null() || json_getter<T>::is(j); }
  static std::optional<std::optional<T>> get(const nlohmann::json& j) {
    if (j.is_null()) return std::optional<T>(std::nullopt);
    auto val = json_getter<T>::get(j);
    if (val) return std::optional<T>(*val);
    return std::nullopt;
  }
};

// Specialization for std::vector
template <typename T>
struct json_getter<std::vector<T>> {
  static bool is(const nlohmann::json& j) { return j.is_array(); }
  static std::optional<std::vector<T>> get(const nlohmann::json& j) {
    if (!is(j)) return std::nullopt;
    std::vector<T> result;
    for (const auto& item : j) {
      auto val = json_getter<T>::get(item);
      if (!val) return std::nullopt;
      result.push_back(*val);
    }
    return result;
  }
};

// Specialization for nlohmann::json::object_t
template <>
struct json_getter<nlohmann::json::object_t> {
  static bool is(const nlohmann::json& j) { return j.is_object(); }
  static std::optional<nlohmann::json::object_t> get(const nlohmann::json& j) {
    if (!is(j)) return std::nullopt;
    return j.get<nlohmann::json::object_t>();
  }
};

// Specialization for nlohmann::json::array_t
template <>
struct json_getter<nlohmann::json::array_t> {
  static bool is(const nlohmann::json& j) { return j.is_array(); }
  static std::optional<nlohmann::json::array_t> get(const nlohmann::json& j) {
    if (!is(j)) return std::nullopt;
    return j.get<nlohmann::json::array_t>();
  }
};

// Check if a JSON value is of type T.
template <typename T>
inline bool json_is(const nlohmann::json& j) {
  return json_getter<T>::is(j);
}

// Check if a JSON object has a key of type T.
template <typename T>
inline bool json_has(const nlohmann::json& j, const std::string& key) {
  if (!j.is_object() || !j.contains(key)) return false;
  return json_is<T>(j.at(key));
}

// Safely get a pointer to a JSON value at a specific key if it exists.
// Returns nullptr if the key is missing.
inline const nlohmann::json* json_at(const nlohmann::json& j, const std::string& key) {
  if (!j.is_object() || !j.contains(key)) return nullptr;
  return &j.at(key);
}

// Safely get a value of type T from a JSON object at a specific key.
// Returns std::nullopt if the key is missing or the type is incorrect.
template <typename T>
inline std::optional<T> json_get(const nlohmann::json& j, const std::string& key) {
  const auto* val = json_at(j, key);
  if (!val) return std::nullopt;
  return json_getter<T>::get(*val);
}

// Safely get a value from a JSON object with a default.
template <typename T>
inline T json_get_or(const nlohmann::json& j, const std::string& key, T default_val) {
  auto val = json_get<T>(j, key);
  return val.has_value() ? *val : default_val;
}

// Safely dump a JSON object to a string without exceptions.
inline std::string json_dump(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace json_utils_internal {

inline constexpr int kMaxSchemaDepth = 64;

inline constexpr char kSchemaType[] = "type";
inline constexpr char kSchemaProperties[] = "properties";
inline constexpr char kSchemaRequired[] = "required";
inline constexpr char kSchemaAdditionalProperties[] = "additionalProperties";
inline constexpr char kSchemaItems[] = "items";
inline constexpr char kSchemaEnum[] = "enum";
inline constexpr std::array<const char*, 6> kAllowedSchemaKeywords = {
    kSchemaType, kSchemaProperties, kSchemaRequired, kSchemaAdditionalProperties, kSchemaItems, kSchemaEnum};

inline constexpr char kSchemaObject[] = "object";
inline constexpr char kSchemaArray[] = "array";
inline constexpr char kSchemaString[] = "string";
inline constexpr char kSchemaNumber[] = "number";
inline constexpr char kSchemaInteger[] = "integer";
inline constexpr char kSchemaBoolean[] = "boolean";
inline constexpr char kSchemaNull[] = "null";
inline constexpr std::array<const char*, 7> kSupportedSchemaTypes = {
    kSchemaObject, kSchemaArray, kSchemaString, kSchemaNumber, kSchemaInteger, kSchemaBoolean, kSchemaNull};

inline bool IsSupportedSchemaType(const std::string& type) {
  for (const char* supported_type : kSupportedSchemaTypes) {
    if (type == supported_type) return true;
  }
  return false;
}

inline bool MatchesSchemaType(const nlohmann::json& value, const std::string& type) {
  if (type == kSchemaObject) return value.is_object();
  if (type == kSchemaArray) return value.is_array();
  if (type == kSchemaString) return value.is_string();
  if (type == kSchemaNumber) return value.is_number();
  if (type == kSchemaInteger) return value.is_number_integer() || value.is_number_unsigned();
  if (type == kSchemaBoolean) return value.is_boolean();
  return value.is_null();
}

inline std::optional<std::string> JsonStringValue(const nlohmann::json& value) {
  return json_getter<std::string>::get(value);
}

inline absl::Status ValidateSchemaNode(const nlohmann::json& schema, const std::string& path, int depth) {
  if (depth > kMaxSchemaDepth) return absl::InvalidArgumentError("JSON Schema nesting exceeds 64 levels");
  if (!schema.is_object()) return absl::InvalidArgumentError(absl::StrCat(path, " must be an object"));

  for (const auto& item : schema.items()) {
    bool allowed = false;
    for (const char* keyword : kAllowedSchemaKeywords) {
      if (item.key() == keyword) {
        allowed = true;
        break;
      }
    }
    if (!allowed) return absl::InvalidArgumentError(absl::StrCat(path, " contains unsupported keyword '", item.key(), "'"));
  }

  const auto type = json_get<std::string>(schema, kSchemaType);
  if (!type || !IsSupportedSchemaType(*type)) {
    return absl::InvalidArgumentError(absl::StrCat(path, " must declare a supported string type"));
  }
  if (const auto* enum_values = json_at(schema, kSchemaEnum); enum_values != nullptr) {
    if (!enum_values->is_array() || enum_values->empty()) {
      return absl::InvalidArgumentError(absl::StrCat(path, ".enum must be a non-empty array"));
    }
    for (const auto& value : *enum_values) {
      if (!MatchesSchemaType(value, *type)) {
        return absl::InvalidArgumentError(absl::StrCat(path, ".enum contains a value that does not match its type"));
      }
    }
  }

  if (*type == kSchemaObject) {
    if (const auto* properties = json_at(schema, kSchemaProperties); properties != nullptr) {
      if (!properties->is_object()) return absl::InvalidArgumentError(absl::StrCat(path, ".properties must be an object"));
      for (const auto& property : properties->items()) {
        const absl::Status status = ValidateSchemaNode(property.value(), absl::StrCat(path, ".properties.", property.key()), depth + 1);
        if (!status.ok()) return status;
      }
    }
    if (const auto* required = json_at(schema, kSchemaRequired); required != nullptr) {
      const auto* properties = json_at(schema, kSchemaProperties);
      if (!required->is_array()) return absl::InvalidArgumentError(absl::StrCat(path, ".required must be an array"));
      for (const auto& name : *required) {
        const auto property_name = JsonStringValue(name);
        if (!property_name) return absl::InvalidArgumentError(absl::StrCat(path, ".required must contain strings"));
        if (properties == nullptr || json_at(*properties, *property_name) == nullptr) {
          return absl::InvalidArgumentError(absl::StrCat(path, ".required references unknown property '", *property_name, "'"));
        }
      }
    }
    if (const auto* additional = json_at(schema, kSchemaAdditionalProperties);
        additional != nullptr && !additional->is_boolean()) {
      return absl::InvalidArgumentError(absl::StrCat(path, ".additionalProperties must be boolean"));
    }
  } else if (*type == kSchemaArray) {
    const auto* items = json_at(schema, kSchemaItems);
    if (items == nullptr) return absl::InvalidArgumentError(absl::StrCat(path, ".items is required for arrays"));
    const absl::Status status = ValidateSchemaNode(*items, absl::StrCat(path, ".items"), depth + 1);
    if (!status.ok()) return status;
  } else if (json_at(schema, kSchemaProperties) != nullptr || json_at(schema, kSchemaRequired) != nullptr ||
             json_at(schema, kSchemaAdditionalProperties) != nullptr || json_at(schema, kSchemaItems) != nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(path, " contains keywords incompatible with type '", *type, "'"));
  }
  return absl::OkStatus();
}

inline absl::Status ValidateValue(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path,
                                  int depth) {
  if (depth > kMaxSchemaDepth) return absl::InvalidArgumentError("JSON value nesting exceeds 64 levels");
  const std::string type = json_get_or(schema, kSchemaType, std::string{});
  if (!MatchesSchemaType(value, type)) return absl::InvalidArgumentError(absl::StrCat(path, " must be ", type));
  if (const auto* enum_values = json_at(schema, kSchemaEnum); enum_values != nullptr) {
    bool matches = false;
    for (const auto& candidate : *enum_values) matches = matches || candidate == value;
    if (!matches) return absl::InvalidArgumentError(absl::StrCat(path, " must match an enum value"));
  }
  if (type == kSchemaObject) {
    const auto* properties = json_at(schema, kSchemaProperties);
    if (const auto* required = json_at(schema, kSchemaRequired); required != nullptr) {
      for (const auto& name : *required) {
        const auto key = json_utils_internal::JsonStringValue(name);
        if (!key) return absl::InvalidArgumentError("Schema required property must be a string");
        if (!value.contains(*key)) {
          return absl::InvalidArgumentError(absl::StrCat(path, " is missing required property '", *key, "'"));
        }
      }
    }
    const bool additional_properties = json_get_or(schema, kSchemaAdditionalProperties, true);
    for (const auto& property : value.items()) {
      const nlohmann::json* property_schema = properties == nullptr ? nullptr : json_at(*properties, property.key());
      if (property_schema == nullptr) {
        if (!additional_properties) return absl::InvalidArgumentError(absl::StrCat(path, " contains unexpected property '", property.key(), "'"));
        continue;
      }
      const absl::Status status = ValidateValue(property.value(), *property_schema, absl::StrCat(path, ".", property.key()), depth + 1);
      if (!status.ok()) return status;
    }
  } else if (type == kSchemaArray) {
    const auto* items = json_at(schema, kSchemaItems);
    for (size_t index = 0; index < value.size(); ++index) {
      const absl::Status status = ValidateValue(value[index], *items, absl::StrCat(path, "[", index, "]"), depth + 1);
      if (!status.ok()) return status;
    }
  }
  return absl::OkStatus();
}

}  // namespace json_utils_internal

// Validates the supported JSON Schema subset used by batch structured output.
inline absl::Status ValidateStructuredOutputSchema(const nlohmann::json& schema) {
  const absl::Status status = json_utils_internal::ValidateSchemaNode(schema, "$", 0);
  if (!status.ok()) return status;
  if (json_get_or(schema, json_utils_internal::kSchemaType, std::string{}) !=
      json_utils_internal::kSchemaObject) {
    return absl::InvalidArgumentError("Structured output schema root type must be object");
  }
  return absl::OkStatus();
}

// Validates a structured output value against an already validated schema.
inline absl::Status ValidateJsonAgainstSchema(const nlohmann::json& value, const nlohmann::json& schema) {
  const absl::Status schema_status = ValidateStructuredOutputSchema(schema);
  if (!schema_status.ok()) return schema_status;
  return json_utils_internal::ValidateValue(value, schema, "$", 0);
}

}  // namespace slop

#endif  // SLOP_CORE_JSON_UTILS_H_
