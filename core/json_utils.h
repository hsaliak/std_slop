#ifndef SLOP_CORE_JSON_UTILS_H_
#define SLOP_CORE_JSON_UTILS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>
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
  static std::optional<T> get(const nlohmann::json& j) {
    if (j.is_null()) return std::nullopt;
    
    // Type checking to avoid exceptions in nlohmann::json::get<T>()
    if constexpr (std::is_same_v<T, std::string>) {
      if (!j.is_string()) return std::nullopt;
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
      if (!j.is_number_integer()) return std::nullopt;
    } else if constexpr (std::is_floating_point_v<T>) {
      if (!j.is_number()) return std::nullopt;
    } else if constexpr (std::is_same_v<T, bool>) {
      if (!j.is_boolean()) return std::nullopt;
    } else if constexpr (std::is_same_v<T, nlohmann::json>) {
      return j;
    }
    
    return j.get<T>();
  }
};

// Specialization for std::optional
template <typename T>
struct json_getter<std::optional<T>> {
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
  static std::optional<std::vector<T>> get(const nlohmann::json& j) {
    if (!j.is_array()) return std::nullopt;
    std::vector<T> result;
    for (const auto& item : j) {
      auto val = json_getter<T>::get(item);
      if (!val) return std::nullopt;
      result.push_back(*val);
    }
    return result;
  }
};

// Safely get a value of type T from a JSON object at a specific key.
// Returns std::nullopt if the key is missing or the type is incorrect.
template <typename T>
inline std::optional<T> json_get(const nlohmann::json& j, const std::string& key) {
  if (!j.is_object() || !j.contains(key)) {
    return std::nullopt;
  }
  return json_getter<T>::get(j.at(key));
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

} // namespace slop

#endif // SLOP_CORE_JSON_UTILS_H_
