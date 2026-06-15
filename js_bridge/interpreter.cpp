
#include "js_bridge/interpreter.h"

#include <string>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "quickjs-libc.h"

namespace slop {
namespace {

constexpr int kMaxRunJsCodeBytes = 256 * 1024;

}  // namespace

void JsInterpreter::JsRuntimeDeleter::operator()(JSRuntime* runtime) const {
  if (runtime != nullptr) {
    JS_FreeRuntime(runtime);
  }
}

void JsInterpreter::JsContextDeleter::operator()(JSContext* context) const {
  if (context != nullptr) {
    JS_FreeContext(context);
  }
}

JsInterpreter::JsInterpreter()
    : runtime_(JS_NewRuntime()), context_(runtime_ == nullptr ? nullptr : JS_NewContext(runtime_.get())) {
  if (runtime_ != nullptr) {
    JS_SetMaxStackSize(runtime_.get(), 1024 * 1024);
  }
  if (context_ != nullptr) {
    js_std_add_helpers(context_.get(), 0, nullptr);
  }
}

JsInterpreter::~JsInterpreter() = default;

absl::StatusOr<nlohmann::json> JsInterpreter::RunJson(std::string code, const std::string& filename) {
  if (runtime_ == nullptr || context_ == nullptr) {
    return absl::InternalError("failed to initialize QuickJS runtime");
  }

  absl::StatusOr<JSValue> value_or = Evaluate(std::move(code), filename);
  if (!value_or.ok()) {
    return value_or.status();
  }
  JSValue value = *value_or;
  absl::Cleanup free_value = [this, value] { JS_FreeValue(context_.get(), value); };
  return ValueToJson(value);
}

absl::StatusOr<JSValue> JsInterpreter::Evaluate(std::string code, const std::string& filename) {
  const std::string wrapped = absl::StrCat("(function(){\n", code, "\n})()");
  JSValue value = JS_Eval(context_.get(), wrapped.c_str(), wrapped.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(value)) {
    return absl::InvalidArgumentError(absl::StrCat("JavaScript error: ", ExceptionMessage()));
  }
  return value;
}

absl::StatusOr<nlohmann::json> JsInterpreter::ValueToJson(JSValueConst value) {
  JSValue stringified = JS_JSONStringify(context_.get(), value, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(stringified)) {
    return absl::InvalidArgumentError(absl::StrCat("JavaScript result is not JSON-serializable: ", ExceptionMessage()));
  }
  absl::Cleanup free_stringified = [this, stringified] { JS_FreeValue(context_.get(), stringified); };

  if (JS_IsUndefined(stringified)) {
    return absl::InvalidArgumentError("JavaScript result is not JSON-serializable");
  }

  const char* raw = JS_ToCString(context_.get(), stringified);
  if (raw == nullptr) {
    return absl::InvalidArgumentError("JavaScript result could not be converted to JSON text");
  }
  absl::Cleanup free_raw = [this, raw] { JS_FreeCString(context_.get(), raw); };

  std::optional<nlohmann::json> parsed = json_parse(raw);
  if (!parsed.has_value()) {
    return absl::InvalidArgumentError("JavaScript result did not produce valid JSON text");
  }
  return *parsed;
}

std::string JsInterpreter::ExceptionMessage() {
  JSValue exception = JS_GetException(context_.get());
  absl::Cleanup free_exception = [this, exception] { JS_FreeValue(context_.get(), exception); };

  const char* raw = JS_ToCString(context_.get(), exception);
  if (raw == nullptr) {
    return "unknown exception";
  }
  absl::Cleanup free_raw = [this, raw] { JS_FreeCString(context_.get(), raw); };
  return raw;
}

absl::StatusOr<nlohmann::json> RunJsForJson(std::string code, const std::string& filename) {
  JsInterpreter interpreter;
  return interpreter.RunJson(std::move(code), filename);
}

absl::Status ValidateRunJsArgs(const nlohmann::json& args) {
  if (!args.is_object()) {
    return absl::InvalidArgumentError("run_js args must be an object");
  }
  const std::optional<std::string> code = json_get<std::string>(args, "code");
  if (!code.has_value()) {
    return absl::InvalidArgumentError("run_js requires string field 'code'");
  }
  if (code->size() > kMaxRunJsCodeBytes) {
    return absl::InvalidArgumentError("run_js code exceeds maximum size");
  }
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> ExecuteRunJsArgs(const nlohmann::json& args) {
  absl::Status status = ValidateRunJsArgs(args);
  if (!status.ok()) return status;
  return RunJsForJson(*json_get<std::string>(args, "code"));
}

}  // namespace slop