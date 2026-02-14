#ifndef SLOP_CORE_LUA_BRIDGE_UTIL_H_
#define SLOP_CORE_LUA_BRIDGE_UTIL_H_

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace slop {

inline nlohmann::json LuaToJSON(sol::object obj) {
  if (obj.is<bool>()) return obj.as<bool>();
  if (obj.is<double>()) {
    double d = obj.as<double>();
    // Check if it's actually an integer to avoid .0 in JSON if possible,
    // though nlohmann::json handles doubles fine.
    return d;
  }
  if (obj.is<std::string>()) return obj.as<std::string>();
  if (obj.is<sol::table>()) {
    sol::table t = obj.as<sol::table>();

    // Check if it's an array (heuristic: keys are all positive integers)
    bool is_array = true;
    size_t count = 0;
    size_t max_idx = 0;
    t.for_each([&](sol::object key, sol::object /*value*/) {
      count++;
      if (!key.is<int>()) {
        is_array = false;
      } else {
        int k = key.as<int>();
        if (k <= 0) {
          is_array = false;
        } else if ((size_t)k > max_idx) {
          max_idx = k;
        }
      }
    });

    // If it's an empty table, we'll treat it as an object by default in Lua -> JSON
    // unless we have a better hint. JSON {} is usually safer than [].
    if (count == 0) return nlohmann::json::object();

    if (is_array && max_idx == count) {
      nlohmann::json j = nlohmann::json::array();
      for (size_t i = 1; i <= max_idx; ++i) {
        j.push_back(LuaToJSON(t[i]));
      }
      return j;
    } else {
      nlohmann::json j = nlohmann::json::object();
      t.for_each([&](sol::object key, sol::object value) {
        std::string k;
        if (key.is<std::string>()) {
          k = key.as<std::string>();
        } else if (key.is<int>()) {
          k = std::to_string(key.as<int>());
        } else {
          // Skip non-string/int keys for JSON objects
          return;
        }
        j[k] = LuaToJSON(value);
      });
      return j;
    }
  }
  return nullptr;
}

inline sol::object JSONToLua(sol::state_view& lua, const nlohmann::json& j) {
  if (j.is_null()) return sol::lua_nil;
  if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
  if (j.is_number_integer()) return sol::make_object(lua, j.get<int64_t>());
  if (j.is_number_unsigned()) return sol::make_object(lua, j.get<uint64_t>());
  if (j.is_number_float()) return sol::make_object(lua, j.get<double>());
  if (j.is_string()) return sol::make_object(lua, j.get<std::string>());
  if (j.is_array()) {
    sol::table t = lua.create_table();
    for (size_t i = 0; i < j.size(); ++i) {
      t[i + 1] = JSONToLua(lua, j[i]);
    }
    return t;
  }
  if (j.is_object()) {
    sol::table t = lua.create_table();
    for (auto& [key, value] : j.items()) {
      t[key] = JSONToLua(lua, value);
    }
    return t;
  }
  return sol::lua_nil;
}

}  // namespace slop

#endif  // SLOP_CORE_LUA_BRIDGE_UTIL_H_
