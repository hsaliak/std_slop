
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

constexpr char kToolsBootstrap[] = R"js(
(function() {
  function requireObject(name, value) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
      throw new TypeError(name + ' args must be an object');
    }
  }

  function requireString(toolName, args, field) {
    if (typeof args[field] !== 'string') {
      throw new TypeError(toolName + ' requires string field ' + field);
    }
  }

  function call(name, args) {
    const safeArgs = args === undefined ? {} : args;
    requireObject(name, safeArgs);
    return globalThis.call_tool(name, safeArgs);
  }

  const helpers = {
    dispatch(name, args = {}) {
      if (typeof name !== 'string' || name.length === 0) {
        throw new TypeError('tools.dispatch requires a non-empty tool name');
      }
      return call(name, args);
    },

    help(args = {}) {
      requireObject('help', args);
      return {
        tools: [
          'dispatch', 'help', 'read_file', 'list_directory', 'grep',
          'llm_query'
        ],
        note: 'Use tools.dispatch(name, args) for host tools without a JS helper.'
      };
    },

    read_file(args = {}) {
      requireObject('read_file', args);
      requireString('read_file', args, 'path');
      return call('read_file', args);
    },

    list_directory(args = {}) {
      requireObject('list_directory', args);
      requireString('list_directory', args, 'path');
      return call('list_directory', args);
    },

    grep(args = {}) {
      requireObject('grep', args);
      requireString('grep', args, 'path');
      requireString('grep', args, 'pattern');
      return call('grep', args);
    },

    llm_query(args = {}) {
      requireObject('llm_query', args);
      requireString('llm_query', args, 'query');
      return call('llm_query', args);
    }
  };

  globalThis.tools = new Proxy(helpers, {
    get(target, property) {
      if (typeof property !== 'string') return undefined;
      if (Object.prototype.hasOwnProperty.call(target, property)) return target[property];
      return function(args = {}) { return globalThis.call_tool(property, args); };
    }
  });
})();
)js";

}  // namespace

struct JsInterpreter::ContextData {
  JsInterpreter* interpreter = nullptr;
};

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

JsInterpreter::JsInterpreter() : JsInterpreter(nullptr) {}

JsInterpreter::JsInterpreter(ToolCaller tool_caller)
    : runtime_(JS_NewRuntime()), context_(runtime_ == nullptr ? nullptr : JS_NewContext(runtime_.get())) {
  tool_caller_ = std::move(tool_caller);
  if (runtime_ != nullptr) {
    JS_SetMaxStackSize(runtime_.get(), 1024 * 1024);
  }
  if (context_ != nullptr) {
    js_std_add_helpers(context_.get(), 0, nullptr);
    context_data_ = std::make_unique<ContextData>(ContextData{this});
    JS_SetContextOpaque(context_.get(), context_data_.get());
    (void)InstallToolBridge();
  }
}

JsInterpreter::~JsInterpreter() {
  if (context_ != nullptr) {
    JS_SetContextOpaque(context_.get(), nullptr);
  }
}

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

JSValue JsInterpreter::JsonToValue(const nlohmann::json& value) {
  const std::string serialized = json_dump(value);
  return JS_ParseJSON(context_.get(), serialized.c_str(), serialized.size(), "<json>");
}

absl::Status JsInterpreter::InstallToolBridge() {
  JSValue global = JS_GetGlobalObject(context_.get());
  absl::Cleanup free_global = [this, global] { JS_FreeValue(context_.get(), global); };

  JSValue call_tool = JS_NewCFunction(
      context_.get(),
      [](JSContext* context, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
        ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(context));
        if (data == nullptr || data->interpreter == nullptr || !data->interpreter->tool_caller_) {
          return JS_ThrowReferenceError(context, "call_tool bridge is not available");
        }
        if (argc != 2) {
          return JS_ThrowTypeError(context, "call_tool requires exactly two arguments");
        }

        const char* raw_name = JS_ToCString(context, argv[0]);
        if (raw_name == nullptr) {
          return JS_ThrowTypeError(context, "call_tool name must be a string");
        }
        absl::Cleanup free_name = [context, raw_name] { JS_FreeCString(context, raw_name); };
        std::string name(raw_name);
        if (name.empty()) {
          return JS_ThrowTypeError(context, "call_tool name must not be empty");
        }
        if (name == "run_js") {
          return JS_ThrowTypeError(context, "call_tool cannot recursively invoke run_js");
        }

        absl::StatusOr<nlohmann::json> args_or = data->interpreter->ValueToJson(argv[1]);
        if (!args_or.ok()) {
          return JS_ThrowTypeError(context, "%s", args_or.status().ToString().c_str());
        }
        if (!args_or->is_object()) {
          return JS_ThrowTypeError(context, "call_tool args must be an object");
        }

        absl::StatusOr<std::string> result_or = data->interpreter->tool_caller_(name, *args_or);
        if (!result_or.ok()) {
          return JS_ThrowInternalError(context, "%s", result_or.status().ToString().c_str());
        }

        nlohmann::json result = *result_or;
        if (std::optional<nlohmann::json> parsed = json_parse(*result_or); parsed.has_value()) {
          result = *parsed;
        }
        JSValue js_result = data->interpreter->JsonToValue(result);
        if (JS_IsException(js_result)) {
          return JS_ThrowInternalError(context, "tool result is not JSON-serializable");
        }
        return js_result;
      },
      "call_tool", 2);
  if (JS_IsException(call_tool)) {
    return absl::InternalError("failed to create call_tool bridge");
  }
  JS_SetPropertyStr(context_.get(), global, "call_tool", call_tool);

  JSValue tools_value = JS_Eval(context_.get(), kToolsBootstrap, std::char_traits<char>::length(kToolsBootstrap),
                                "<tools_bootstrap>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(tools_value)) {
    return absl::InternalError(absl::StrCat("failed to install tools bridge: ", ExceptionMessage()));
  }
  JS_FreeValue(context_.get(), tools_value);
  return absl::OkStatus();
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

absl::StatusOr<nlohmann::json> RunJsForJson(std::string code, JsInterpreter::ToolCaller tool_caller,
                                            const std::string& filename) {
  JsInterpreter interpreter(std::move(tool_caller));
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

absl::StatusOr<nlohmann::json> ExecuteRunJsArgs(const nlohmann::json& args, JsInterpreter::ToolCaller tool_caller) {
  absl::Status status = ValidateRunJsArgs(args);
  if (!status.ok()) return status;
  return RunJsForJson(*json_get<std::string>(args, "code"), std::move(tool_caller));
}

}  // namespace slop