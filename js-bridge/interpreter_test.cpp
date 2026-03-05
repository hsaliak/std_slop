#include "js-bridge/interpreter.h"

#include <gtest/gtest.h>

namespace slop {
namespace {

/**
 * Convert a JSValue number to int32 for assertions.
 */
int32_t ToInt32(JSContext* ctx, JSValueConst value) {
  int32_t out = 0;
  EXPECT_EQ(JS_ToInt32(ctx, &out, value), 0);
  return out;
}

TEST(JsInterpreterTest, RunStringWrappedReturnsResult) {
  JsInterpreter interpreter;

  JSValue result = interpreter.RunString("return 6 * 7;", "wrapped.js", true);
  ASSERT_FALSE(JS_IsException(result));
  EXPECT_TRUE(JS_IsNumber(result));
  EXPECT_EQ(ToInt32(interpreter.context(), result), 42);

  JS_FreeValue(interpreter.context(), result);
}

TEST(JsInterpreterTest, RunStringUnwrappedEvaluatesGlobalCode) {
  JsInterpreter interpreter;

  JSValue result = interpreter.RunString("1 + 2", "plain.js", false);
  ASSERT_FALSE(JS_IsException(result));
  EXPECT_TRUE(JS_IsNumber(result));
  EXPECT_EQ(ToInt32(interpreter.context(), result), 3);

  JS_FreeValue(interpreter.context(), result);
}

TEST(JsInterpreterTest, RunStringWrappedSyntaxErrorReturnsException) {
  JsInterpreter interpreter;

  JSValue result = interpreter.RunString("return ;", "bad.js", true);
  ASSERT_TRUE(JS_IsException(result));

  JSValue ex = JS_GetException(interpreter.context());
  ASSERT_FALSE(JS_IsUndefined(ex));

  const char* msg = JS_ToCString(interpreter.context(), ex);
  ASSERT_NE(msg, nullptr);
  std::string message(msg);
  JS_FreeCString(interpreter.context(), msg);
  JS_FreeValue(interpreter.context(), ex);

  EXPECT_NE(message.find("SyntaxError"), std::string::npos);
}

}  // namespace
}  // namespace slop

