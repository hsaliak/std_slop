#include "core/lua_bridge_util.h"
#include <gtest/gtest.h>
#include <sol/sol.hpp>

namespace slop {

TEST(LuaBridgeUtilTest, BasicTypes) {
  sol::state lua;
  lua.open_libraries(sol::lib::base);

  EXPECT_TRUE(LuaToJSON(sol::make_object(lua, 123)).is_number());
  EXPECT_TRUE(LuaToJSON(sol::make_object(lua, true)).is_boolean());
  EXPECT_EQ(LuaToJSON(sol::make_object(lua, "hello")).get<std::string>(), "hello");
  EXPECT_TRUE(LuaToJSON(sol::lua_nil).is_null());
}

TEST(LuaBridgeUtilTest, ArrayTable) {
  sol::state lua;
  lua.script("t = {10, 20, 30}");
  sol::table t = lua["t"];
  nlohmann::json j = LuaToJSON(t);
  ASSERT_TRUE(j.is_array());
  EXPECT_EQ(j.size(), 3);
  EXPECT_EQ(j[0], 10.0);
}

TEST(LuaBridgeUtilTest, ObjectTable) {
  sol::state lua;
  lua.script("t = {a = 1, b = 2}");
  sol::table t = lua["t"];
  nlohmann::json j = LuaToJSON(t);
  ASSERT_TRUE(j.is_object());
  EXPECT_EQ(j["a"], 1.0);
  EXPECT_EQ(j["b"], 2.0);
}

TEST(LuaBridgeUtilTest, CircularReference) {
  sol::state lua;
  lua.script("t = {}; t.a = t");
  sol::table t = lua["t"];
  // This should no longer crash.
  nlohmann::json j = LuaToJSON(t);
  EXPECT_EQ(j["a"], "<<cycle>>");
}

TEST(LuaBridgeUtilTest, DeeplyNested) {
  sol::state lua;
  lua.script("t = {}; local curr = t; for i=1,100 do curr.a = {}; curr = curr.a end");
  sol::table t = lua["t"];
  nlohmann::json j = LuaToJSON(t);
  // Should not crash, but will be truncated at depth 64
  int depth = 0;
  nlohmann::json* curr = &j;
  while (curr->contains("a") && (*curr)["a"].is_object()) {
    curr = &((*curr)["a"]);
    depth++;
  }
  EXPECT_LE(depth, 65);
}

TEST(LuaBridgeUtilTest, InvalidUTF8) {
  sol::state lua;
  lua.script("s = '\\xff'");
  sol::object s = lua["s"];
  nlohmann::json j = LuaToJSON(s);
  // SafeDump should not crash
  std::string dumped = SafeDump(j);
  EXPECT_FALSE(dumped.empty());
}

}  // namespace slop
