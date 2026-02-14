#include "lua-bridge/interpreter.h"

#include <gtest/gtest.h>

namespace slop {

TEST(LuaIntegrationTest, BasicGlobals) {
  Interpreter interp;
  auto result = interp.RunString("y = 100");
  EXPECT_TRUE(result.valid());

  int y = interp.state()["y"];
  EXPECT_EQ(y, 100);
}

TEST(LuaIntegrationTest, CallLuaFunction) {
  Interpreter interp;
  auto result = interp.RunFile("lua-bridge/test_script.lua");
  ASSERT_TRUE(result.valid());

  sol::function add = interp.state()["add"];
  int sum = add(10, 20);
  EXPECT_EQ(sum, 30);
}

TEST(LuaIntegrationTest, CallCppFunction) {
  Interpreter interp;

  // Bind a C++ function to Lua
  interp.state()["cpp_function"] = [](int val) { return val * 2; };

  auto result = interp.RunFile("lua-bridge/test_script.lua");
  ASSERT_TRUE(result.valid());

  sol::function call_cpp = interp.state()["call_cpp"];
  int val = call_cpp(21);
  EXPECT_EQ(val, 42);
}

TEST(LuaIntegrationTest, StdLibsLoaded) {
  Interpreter interp;

  // Test math lib
  auto math_res = interp.RunString("return math.sqrt(16)");
  EXPECT_TRUE(math_res.valid());
  double sqrt_val = math_res;
  EXPECT_DOUBLE_EQ(sqrt_val, 4.0);

  // Test io/os libs (just check existence)
  auto io_res = interp.RunString("return type(io.open)");
  EXPECT_TRUE(io_res.valid());
  std::string io_type = io_res;
  EXPECT_EQ(io_type, "function");
}

}  // namespace slop
