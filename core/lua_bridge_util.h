#ifndef SLOP_CORE_LUA_BRIDGE_UTIL_H_
#define SLOP_CORE_LUA_BRIDGE_UTIL_H_

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace slop {

namespace detail {

inline nlohmann::json LuaToJSONInternal(sol::object obj, int depth, std::vector<const void*>& visited) {
  if (depth > 64) return nullptr;
  if (!obj.valid() || obj.is<sol::lua_nil_t>()) return nullptr;

  if (obj.is<bool>()) return obj.as<bool>();
  if (obj.is<double>()) return obj.as<double>();
  if (obj.is<std::string>()) return obj.as<std::string>();

  if (obj.is<sol::table>()) {
    sol::table t = obj.as<sol::table>();
    const void* ptr = t.pointer();
    if (std::find(visited.begin(), visited.end(), ptr) != visited.end()) return "<<cycle>>";
    visited.push_back(ptr);

    // Collect items first to avoid recursing inside for_each
    std::vector<std::pair<sol::object, sol::object>> items;
    t.for_each([&](sol::object k, sol::object v) { items.push_back({k, v}); });

    size_t count = items.size();
    size_t max_idx = 0;
    bool is_array = true;
    for (auto& item : items) {
      if (item.first.is<int>()) {
        int idx = item.first.as<int>();
        if (idx > 0) {
          if (static_cast<size_t>(idx) > max_idx) max_idx = idx;
        } else {
          is_array = false;
        }
      } else {
        is_array = false;
      }
    }

    nlohmann::json j;
    if (count == 0) {
      j = nlohmann::json::object();
    } else if (is_array && max_idx == count) {
      j = nlohmann::json::array();
      // For arrays, we need to ensure correct order
      // So we'll just use t[i] again, but it's safe now because we're not inside for_each
      for (size_t i = 1; i <= max_idx; ++i) {
        j.push_back(LuaToJSONInternal(t[i], depth + 1, visited));
      }
    } else {
      j = nlohmann::json::object();
      for (auto& item : items) {
        std::string k;
        if (item.first.is<std::string>()) {
          k = item.first.as<std::string>();
        } else if (item.first.is<int>()) {
          k = std::to_string(item.first.as<int>());
        } else {
          continue;
        }
        j[k] = LuaToJSONInternal(item.second, depth + 1, visited);
      }
    }

    visited.pop_back();
    return j;
  }
  return nullptr;
}

}  // namespace detail

inline nlohmann::json LuaToJSON(sol::object obj) {
  std::vector<const void*> visited;
  return detail::LuaToJSONInternal(obj, 0, visited);
}

inline std::string SafeDump(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline sol::object JSONToLua(sol::state_view lua, const nlohmann::json& j) {
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
