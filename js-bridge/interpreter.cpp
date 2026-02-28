#include "js-bridge/interpreter.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include "core/shell_util.h"
#include "core/tool_dispatcher.h"
#include "absl/log/log.h"

namespace slop {

struct ContextData {
  std::stringstream* stdout_buffer;
  std::shared_ptr<CancellationRequest> cancellation;
  const absl::flat_hash_map<std::string, std::function<absl::StatusOr<std::string>(const nlohmann::json&, std::shared_ptr<CancellationRequest>)>>* dispatch_map;
  JsInterpreter* interpreter;
  ToolDispatcher* dispatcher;
};

JsInterpreter::JsInterpreter() {
  rt_ = JS_NewRuntime();
  ctx_ = JS_NewContext(rt_);
}

JsInterpreter::~JsInterpreter() {
  ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx_));
  delete data;
  JS_FreeContext(ctx_);
  JS_FreeRuntime(rt_);
}

JSValue JsInterpreter::RunString(const std::string& code, const std::string& filename) {
  JSValue val = JS_Eval(ctx_, code.c_str(), code.length(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
  return val;
}

JSValue JsInterpreter::RunFile(const std::string& path) {
  std::ifstream t(path);
  if (!t.is_open()) {
    return JS_EXCEPTION;
  }
  std::stringstream buffer;
  buffer << t.rdbuf();
  return RunString(buffer.str(), path);
}

JSValue JsInterpreter::JSONToJS(const nlohmann::json& j) {
  std::string s = j.dump();
  return JS_ParseJSON(ctx_, s.c_str(), s.length(), "json");
}

nlohmann::json JsInterpreter::JSToJSON(JSValue val) {
  JSValue str_val = JS_JSONStringify(ctx_, val, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(str_val)) return nullptr;
  const char* str = JS_ToCString(ctx_, str_val);
  nlohmann::json j = nlohmann::json::parse(str, nullptr, false);
  if (j.is_discarded()) {
    j = nlohmann::json();
  }
  JS_FreeCString(ctx_, str);
  JS_FreeValue(ctx_, str_val);
  return j;
}

static JSValue js_print(JSContext* ctx, [[maybe_unused]] JSValueConst this_val, int argc, JSValueConst* argv) {
  ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
  std::stringstream ss;
  for (int i = 0; i < argc; i++) {
    const char* str = JS_ToCString(ctx, argv[i]);
    if (str) {
      ss << str << (i == argc - 1 ? "" : "\t");
      JS_FreeCString(ctx, str);
    }
  }
  LOG(INFO) << "[JS] " << ss.str();
  if (data && data->stdout_buffer) {
    *(data->stdout_buffer) << ss.str() << "\n";
  }
  return JS_UNDEFINED;
}

static JSValue js_os_run(JSContext* ctx, [[maybe_unused]] JSValueConst this_val, int argc, JSValueConst* argv) {
  ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
  if (argc < 1) return JS_EXCEPTION;
  const char* command = JS_ToCString(ctx, argv[0]);
  auto res_or = RunCommand(command, data ? data->cancellation : nullptr);
  JS_FreeCString(ctx, command);

  JSValue obj = JS_NewObject(ctx);
  if (!res_or.ok()) {
    JS_SetPropertyStr(ctx, obj, "stdout", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "stderr", JS_NewString(ctx, res_or.status().ToString().c_str()));
    JS_SetPropertyStr(ctx, obj, "exit_code", JS_NewInt32(ctx, -1));
  } else {
    JS_SetPropertyStr(ctx, obj, "stdout", JS_NewString(ctx, res_or->stdout_out.c_str()));
    JS_SetPropertyStr(ctx, obj, "stderr", JS_NewString(ctx, res_or->stderr_out.c_str()));
    JS_SetPropertyStr(ctx, obj, "exit_code", JS_NewInt32(ctx, res_or->exit_code));
  }
  return obj;
}

static JSValue js_dispatch_async(JSContext* ctx, [[maybe_unused]] JSValueConst this_val, int argc, JSValueConst* argv) {
  ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
  if (!data || !data->dispatcher) return JS_ThrowReferenceError(ctx, "Dispatcher not available");
  if (argc < 2) return JS_EXCEPTION;

  const char* name = JS_ToCString(ctx, argv[0]);
  nlohmann::json args = data->interpreter->JSToJSON(argv[1]);

  ToolDispatcher::Call call;
  call.id = "async-" + std::string(name);
  call.name = name;
  call.args = args;
  
  auto job = data->dispatcher->Submit(call, data->cancellation);
  JS_FreeCString(ctx, name);

  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "_job_ptr", JS_NewInt64(ctx, (int64_t)job.get()));
  
  auto is_ready = [](JSContext* ctx, JSValueConst this_val, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv) -> JSValue {
    JSValue ptr_val = JS_GetPropertyStr(ctx, this_val, "_job_ptr");
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, ptr_val);
    JS_FreeValue(ctx, ptr_val);
    ToolJob* job = reinterpret_cast<ToolJob*>(ptr);
    return JS_NewBool(ctx, job->IsReady());
  };

  auto wait = [](JSContext* ctx, JSValueConst this_val, [[maybe_unused]] int argc, [[maybe_unused]] JSValueConst* argv) -> JSValue {
    JSValue ptr_val = JS_GetPropertyStr(ctx, this_val, "_job_ptr");
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, ptr_val);
    JS_FreeValue(ctx, ptr_val);
    ToolJob* job = reinterpret_cast<ToolJob*>(ptr);
    auto res = job->Wait();
    if (!res.ok()) return JS_ThrowInternalError(ctx, "Job failed: %s", res.status().ToString().c_str());
    return JS_NewString(ctx, res->c_str());
  };

  JS_SetPropertyStr(ctx, obj, "is_ready", JS_NewCFunction(ctx, is_ready, "is_ready", 0));
  JS_SetPropertyStr(ctx, obj, "wait", JS_NewCFunction(ctx, wait, "wait", 0));

  return obj;
}

void JsInterpreter::InitializeEnvironment(
    [[maybe_unused]] Database* db, ToolDispatcher* dispatcher,
    std::shared_ptr<CancellationRequest> cancellation,
    const absl::flat_hash_map<std::string, std::function<absl::StatusOr<std::string>(const nlohmann::json&, std::shared_ptr<CancellationRequest>)>>& dispatch_map,
    std::stringstream& stdout_buffer) {
  
  ContextData* data = new ContextData{&stdout_buffer, cancellation, &dispatch_map, this, dispatcher};
  JS_SetContextOpaque(ctx_, data);

  JSValue global_obj = JS_GetGlobalObject(ctx_);

  JS_SetPropertyStr(ctx_, global_obj, "print", JS_NewCFunction(ctx_, js_print, "print", 1));
  JS_SetPropertyStr(ctx_, global_obj, "__os_run", JS_NewCFunction(ctx_, js_os_run, "__os_run", 1));

  JSValue tools_obj = JS_NewObject(ctx_);
  JS_SetPropertyStr(ctx_, tools_obj, "dispatch_async", JS_NewCFunction(ctx_, js_dispatch_async, "dispatch_async", 2));

  for (auto const& [name, handler] : dispatch_map) {
    if (name == "run_lua" || name == "run_js") continue;
    
    auto tool_wrapper = [](JSContext* ctx, [[maybe_unused]] JSValueConst this_val, [[maybe_unused]] int argc, JSValueConst* argv, [[maybe_unused]] int magic, JSValue* func_data) -> JSValue {
      ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
      const char* name = JS_ToCString(ctx, func_data[0]);
      nlohmann::json args = data->interpreter->JSToJSON(argv[0]);
      
      auto it = data->dispatch_map->find(name);
      if (it == data->dispatch_map->end()) {
          JS_FreeCString(ctx, name);
          return JS_ThrowReferenceError(ctx, "Tool not found: %s", name);
      }
      
      auto result = it->second(args, data->cancellation);
      JS_FreeCString(ctx, name);
      
      if (!result.ok()) {
          return JS_ThrowInternalError(ctx, "Error: %s", result.status().ToString().c_str());
      }
      return JS_NewString(ctx, result->c_str());
    };

    JSValue name_val = JS_NewString(ctx_, name.c_str());
    JSValue func = JS_NewCFunctionData(ctx_, tool_wrapper, 1, 0, 1, &name_val);
    JS_SetPropertyStr(ctx_, tools_obj, name.c_str(), func);
    JS_FreeValue(ctx_, name_val);
  }
  
  JS_SetPropertyStr(ctx_, global_obj, "tools", tools_obj);
  JS_FreeValue(ctx_, global_obj);
}

}  // namespace slop

