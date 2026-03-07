#include "js-bridge/interpreter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#include "absl/cleanup/cleanup.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "core/shell_util.h"
#include "core/tool_dispatcher.h"
#include "quickjs-libc.h"

namespace {
std::string CorrectLineNumbers(const std::string& error_msg, int line_offset) {
  std::regex line_re(R"((?:line |:)(\d+)(?::|\b))");
  std::smatch match;
  std::string::const_iterator searchStart(error_msg.cbegin());
  std::string temp;

  while (std::regex_search(searchStart, error_msg.cend(), match, line_re)) {
    absl::StrAppend(&temp, match.prefix().str());
    int original_line = std::stoi(match[1].str());
    int corrected_line = std::max(1, original_line - line_offset);

    std::string prefix_str = match[0].str();
    size_t num_pos = prefix_str.find(match[1].str());
    absl::StrAppend(&temp, prefix_str.substr(0, num_pos), corrected_line,
                    prefix_str.substr(num_pos + match[1].length()));

    searchStart = match.suffix().first;
  }
  absl::StrAppend(&temp, std::string(searchStart, error_msg.cend()));
  return temp;
}
}  // namespace

namespace slop {

struct ContextData {
  std::stringstream* stdout_buffer;
  std::shared_ptr<CancellationRequest> cancellation;
  const absl::flat_hash_map<std::string, std::function<absl::StatusOr<std::string>(
                                             const nlohmann::json&, std::shared_ptr<CancellationRequest>)>>*
      dispatch_map;
  JsInterpreter* interpreter;
  ToolDispatcher* dispatcher;
  std::vector<std::shared_ptr<ToolJob>> active_jobs;
};

JsInterpreter::JsInterpreter() {
  rt_ = JS_NewRuntime();
  js_std_init_handlers(rt_);
  ctx_ = JS_NewContext(rt_);
  JS_SetModuleLoaderFunc(rt_, nullptr, js_module_loader, nullptr);
}

JsInterpreter::~JsInterpreter() {
  std::unique_ptr<ContextData> data(static_cast<ContextData*>(JS_GetContextOpaque(ctx_)));
  JS_FreeContext(ctx_);
  js_std_free_handlers(rt_);
  JS_FreeRuntime(rt_);
}

JSValue JsInterpreter::RunString(const std::string& code, const std::string& filename, bool wrap) {
  if (!wrap) {
    return JS_Eval(ctx_, code.c_str(), code.length(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
  }

  // 1. Wrap the code in an async IIFE
  std::string prefix =
      "globalThis.__agent_res = undefined;\n"
      "globalThis.__agent_err = undefined;\n"
      "(async () => {\n"
      "  try {\n"
      "    globalThis.__agent_res = await (async () => {\n"
      "      ";
  std::string suffix =
      "\n"
      "    })();\n"
      "  } catch (e) {\n"
      "    globalThis.__agent_err = e;\n"
      "  }\n"
      "})();";

  std::string wrapped_code = prefix + code + suffix;
  int line_offset = std::count(prefix.begin(), prefix.end(), '\n');

  // 2. Compile the wrapped code
  JSValue compiled_func = JS_Eval(ctx_, wrapped_code.c_str(), wrapped_code.length(), filename.c_str(),
                                  JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(compiled_func)) {
    JSValue exception = JS_GetException(ctx_);
    absl::Cleanup free_exception = [this, exception] { JS_FreeValue(ctx_, exception); };

    const char* err_str = JS_ToCString(ctx_, exception);
    absl::Cleanup free_err_str = [this, err_str] {
      if (err_str) JS_FreeCString(ctx_, err_str);
    };
    std::string err_msg = err_str ? err_str : "Unknown Syntax Error";

    JSValue stack = JS_GetPropertyStr(ctx_, exception, "stack");
    absl::Cleanup free_stack = [this, stack] { JS_FreeValue(ctx_, stack); };

    if (!JS_IsUndefined(stack)) {
      const char* stack_str = JS_ToCString(ctx_, stack);
      absl::Cleanup free_stack_str = [this, stack_str] {
        if (stack_str) JS_FreeCString(ctx_, stack_str);
      };
      if (stack_str) {
        absl::StrAppend(&err_msg, "\n", stack_str);
      }
    }

    std::string corrected_error = CorrectLineNumbers(err_msg, line_offset);
    return JS_ThrowTypeError(ctx_, "%s", corrected_error.c_str());
  }

  // 3. Execute the compiled function
  JSValue eval_res = JS_EvalFunction(ctx_, compiled_func);
  if (JS_IsException(eval_res)) {
    JSValue exception = JS_GetException(ctx_);
    absl::Cleanup free_exception = [this, exception] { JS_FreeValue(ctx_, exception); };

    const char* err_str = JS_ToCString(ctx_, exception);
    absl::Cleanup free_err_str = [this, err_str] {
      if (err_str) JS_FreeCString(ctx_, err_str);
    };
    std::string err_msg = err_str ? err_str : "Unknown Syntax Error";

    JSValue stack = JS_GetPropertyStr(ctx_, exception, "stack");
    absl::Cleanup free_stack = [this, stack] { JS_FreeValue(ctx_, stack); };

    if (!JS_IsUndefined(stack)) {
      const char* stack_str = JS_ToCString(ctx_, stack);
      absl::Cleanup free_stack_str = [this, stack_str] {
        if (stack_str) JS_FreeCString(ctx_, stack_str);
      };
      if (stack_str) {
        absl::StrAppend(&err_msg, "\n", stack_str);
      }
    }

    std::string corrected_error = CorrectLineNumbers(err_msg, line_offset);
    return JS_ThrowTypeError(ctx_, "%s", corrected_error.c_str());
  }
  JS_FreeValue(ctx_, eval_res);  // Free the unresolved Promise, we don't need it

  // 3. Pump the QuickJS Event Loop!
  // This is required for `await` to actually finish executing in QuickJS.
  JSRuntime* rt = JS_GetRuntime(ctx_);
  JSContext* pctx;
  int err;
  while ((err = JS_ExecutePendingJob(rt, &pctx)) > 0) {
    // Loop runs until all microtasks (promises) are resolved
  }

  if (err < 0) {
    // Event loop crashed (rare, usually out of memory)
    return JS_GetException(pctx);
  }

  // 4. Extract the final resolved value from the global object
  JSValue global_obj = JS_GetGlobalObject(ctx_);

  absl::Cleanup free_global = [this, global_obj] { JS_FreeValue(ctx_, global_obj); };

  // Did the script throw an error during execution?
  JSValue err_val = JS_GetPropertyStr(ctx_, global_obj, "__agent_err");
  if (!JS_IsUndefined(err_val)) {
    const char* err_str = JS_ToCString(ctx_, err_val);
    absl::Cleanup free_err_str = [this, err_str] {
      if (err_str) JS_FreeCString(ctx_, err_str);
    };
    std::string err_msg = err_str ? err_str : "Unknown Error";

    JSValue stack = JS_GetPropertyStr(ctx_, err_val, "stack");
    absl::Cleanup free_stack = [this, stack] { JS_FreeValue(ctx_, stack); };

    if (!JS_IsUndefined(stack)) {
      const char* stack_str = JS_ToCString(ctx_, stack);
      absl::Cleanup free_stack_str = [this, stack_str] {
        if (stack_str) JS_FreeCString(ctx_, stack_str);
      };
      if (stack_str) {
        absl::StrAppend(&err_msg, "\n", stack_str);
      }
    }

    std::string corrected_error = CorrectLineNumbers(err_msg, line_offset);
    JS_FreeValue(ctx_, err_val);
    return JS_ThrowTypeError(ctx_, "%s", corrected_error.c_str());
  }
  absl::Cleanup free_err = [this, err_val] { JS_FreeValue(ctx_, err_val); };

  // Grab the successful return value
  JSValue res_val = JS_GetPropertyStr(ctx_, global_obj, "__agent_res");

  // Return the actual value your JS payload sent back!
  return res_val;
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
  absl::Cleanup free_str_val = [this, str_val] { JS_FreeValue(ctx_, str_val); };
  const char* str = JS_ToCString(ctx_, str_val);
  if (!str) return nullptr;
  absl::Cleanup free_str = [this, str] { JS_FreeCString(ctx_, str); };
  auto j_opt = slop::json_parse(str);
  return j_opt ? *j_opt : nlohmann::json();
}

static JSValue js_print(JSContext* ctx, [[maybe_unused]] JSValueConst this_val, int argc, JSValueConst* argv) {
  ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
  std::stringstream ss;
  for (int i = 0; i < argc; i++) {
    const char* str = JS_ToCString(ctx, argv[i]);
    if (str) {
      absl::Cleanup free_str = [ctx, str] { JS_FreeCString(ctx, str); };
      ss << str << (i == argc - 1 ? "" : "\t");
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
  if (!command) return JS_EXCEPTION;
  absl::Cleanup free_command = [ctx, command] { JS_FreeCString(ctx, command); };
  auto res_or = RunCommand(command, data ? data->cancellation : nullptr);

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
  if (!name) return JS_EXCEPTION;
  absl::Cleanup free_name = [ctx, name] { JS_FreeCString(ctx, name); };
  nlohmann::json args = data->interpreter->JSToJSON(argv[1]);

  ToolDispatcher::Call call;
  call.id = "async-" + std::string(name);
  call.name = name;
  call.args = args;

  auto job = data->dispatcher->Submit(call, data->cancellation);
  data->active_jobs.push_back(job);

  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "_job_ptr", JS_NewInt64(ctx, reinterpret_cast<int64_t>(job.get())));

  auto is_ready = [](JSContext* ctx, JSValueConst this_val, [[maybe_unused]] int argc,
                     [[maybe_unused]] JSValueConst* argv) -> JSValue {
    JSValue ptr_val = JS_GetPropertyStr(ctx, this_val, "_job_ptr");
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, ptr_val);
    JS_FreeValue(ctx, ptr_val);
    ToolJob* job = reinterpret_cast<ToolJob*>(ptr);
    return JS_NewBool(ctx, job->IsReady());
  };

  auto wait = [](JSContext* ctx, JSValueConst this_val, [[maybe_unused]] int argc,
                 [[maybe_unused]] JSValueConst* argv) -> JSValue {
    JSValue ptr_val = JS_GetPropertyStr(ctx, this_val, "_job_ptr");
    int64_t ptr;
    JS_ToInt64(ctx, &ptr, ptr_val);
    JS_FreeValue(ctx, ptr_val);
    ToolJob* job = reinterpret_cast<ToolJob*>(ptr);
    auto res = job->Wait();
    if (!res.ok()) return JS_ThrowInternalError(ctx, "Job failed: %s", res.status().ToString().c_str());
    auto envelope = nlohmann::json::parse(*res, nullptr, false);
    if (!envelope.is_discarded() && envelope.is_object()) {
      auto ok_it = envelope.find("ok");
      if (ok_it != envelope.end() && ok_it->is_boolean()) {
        if (!ok_it->get<bool>()) {
          std::string err = "Tool failed";
          auto error_it = envelope.find("error");
          if (error_it != envelope.end()) {
            if (error_it->is_object()) {
              auto msg_it = error_it->find("message");
              if (msg_it != error_it->end() && msg_it->is_string()) {
                err = msg_it->get<std::string>();
              } else {
                err = error_it->dump();
              }
            } else if (error_it->is_string()) {
              err = error_it->get<std::string>();
            }
          }
          return JS_ThrowInternalError(ctx, "%s", err.c_str());
        }
        auto result_it = envelope.find("result");
        if (result_it == envelope.end() || result_it->is_null()) {
          return JS_NewString(ctx, "");
        }
        if (result_it->is_string()) {
          const std::string s = result_it->get<std::string>();
          return JS_NewString(ctx, s.c_str());
        }
        const std::string s = result_it->dump();
        return JS_NewString(ctx, s.c_str());
      }
    }
    return JS_NewString(ctx, res->c_str());
  };

  JS_SetPropertyStr(ctx, obj, "is_ready", JS_NewCFunction(ctx, is_ready, "is_ready", 0));
  JS_SetPropertyStr(ctx, obj, "wait", JS_NewCFunction(ctx, wait, "wait", 0));

  return obj;
}

void JsInterpreter::InitializeEnvironment(
    [[maybe_unused]] Database* db, ToolDispatcher* dispatcher, std::shared_ptr<CancellationRequest> cancellation,
    const absl::flat_hash_map<std::string, std::function<absl::StatusOr<std::string>(
                                               const nlohmann::json&, std::shared_ptr<CancellationRequest>)>>&
        dispatch_map,
    std::stringstream& stdout_buffer) {
  auto data = std::make_unique<ContextData>();
  data->stdout_buffer = &stdout_buffer;
  data->cancellation = cancellation;
  data->dispatch_map = &dispatch_map;
  data->interpreter = this;
  data->dispatcher = dispatcher;
  JS_SetContextOpaque(ctx_, data.release());

  JSValue global_obj = JS_GetGlobalObject(ctx_);

  JS_SetPropertyStr(ctx_, global_obj, "print", JS_NewCFunction(ctx_, js_print, "print", 1));
  JS_SetPropertyStr(ctx_, global_obj, "__os_run", JS_NewCFunction(ctx_, js_os_run, "__os_run", 1));

  JSValue console_obj = JS_NewObject(ctx_);
  JS_SetPropertyStr(ctx_, console_obj, "log", JS_NewCFunction(ctx_, js_print, "log", 1));
  JS_SetPropertyStr(ctx_, console_obj, "info", JS_NewCFunction(ctx_, js_print, "info", 1));
  JS_SetPropertyStr(ctx_, console_obj, "warn", JS_NewCFunction(ctx_, js_print, "warn", 1));
  JS_SetPropertyStr(ctx_, console_obj, "error", JS_NewCFunction(ctx_, js_print, "error", 1));
  JS_SetPropertyStr(ctx_, global_obj, "console", console_obj);

  js_init_module_std(ctx_, "std");
  js_init_module_os(ctx_, "os");

  constexpr const char* kQuickJsGlobals =
      "import * as std from 'std';\n"
      "import * as os from 'os';\n"
      "globalThis.std = std;\n"
      "globalThis.os = os;\n";
  JSValue module_res =
      JS_Eval(ctx_, kQuickJsGlobals, std::strlen(kQuickJsGlobals), "<quickjs-globals>", JS_EVAL_TYPE_MODULE);
  if (JS_IsException(module_res)) {
    JSValue exception = JS_GetException(ctx_);
    const char* err_cstr = JS_ToCString(ctx_, exception);
    std::string err = err_cstr ? err_cstr : "unknown error";
    if (err_cstr) {
      JS_FreeCString(ctx_, err_cstr);
    }
    JS_FreeValue(ctx_, exception);
    JS_FreeValue(ctx_, module_res);
    JS_FreeValue(ctx_, global_obj);
    LOG(FATAL) << "Failed to expose QuickJS std/os modules: " << err;
  }
  JS_FreeValue(ctx_, module_res);

  JSValue tools_obj = JS_NewObject(ctx_);

  JS_SetPropertyStr(ctx_, tools_obj, "dispatch_async", JS_NewCFunction(ctx_, js_dispatch_async, "dispatch_async", 2));

  auto js_check_syntax = [](JSContext* ctx, [[maybe_unused]] JSValueConst this_val, int argc,
                            JSValueConst* argv) -> JSValue {
    if (argc < 1 || !JS_IsString(argv[0])) {
      return JS_ThrowTypeError(ctx, "Expected a string argument");
    }

    int line_offset = 0;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
      JS_ToInt32(ctx, &line_offset, argv[1]);
    }

    const char* code_cstr = JS_ToCString(ctx, argv[0]);
    if (!code_cstr) return JS_EXCEPTION;
    absl::Cleanup free_code_cstr = [ctx, code_cstr] { JS_FreeCString(ctx, code_cstr); };
    std::string code(code_cstr);

    JSValue compiled = JS_Eval(ctx, code.c_str(), code.length(), "<persist_function>",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    absl::Cleanup free_compiled = [ctx, compiled] { JS_FreeValue(ctx, compiled); };

    if (JS_IsException(compiled)) {
      JSValue exception = JS_GetException(ctx);
      absl::Cleanup free_exception = [ctx, exception] { JS_FreeValue(ctx, exception); };

      const char* err_str = JS_ToCString(ctx, exception);
      absl::Cleanup free_err_str = [ctx, err_str] {
        if (err_str) JS_FreeCString(ctx, err_str);
      };
      std::string err_msg = err_str ? err_str : "Unknown Syntax Error";

      JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
      absl::Cleanup free_stack = [ctx, stack] { JS_FreeValue(ctx, stack); };

      if (!JS_IsUndefined(stack)) {
        const char* stack_str = JS_ToCString(ctx, stack);
        absl::Cleanup free_stack_str = [ctx, stack_str] {
          if (stack_str) JS_FreeCString(ctx, stack_str);
        };
        if (stack_str) {
          absl::StrAppend(&err_msg, "\n", stack_str);
        }
      }

      std::string corrected_error = CorrectLineNumbers(err_msg, line_offset);
      return JS_ThrowTypeError(ctx, "Syntax Error: %s", corrected_error.c_str());
    }

    return JS_UNDEFINED;
  };
  JS_SetPropertyStr(ctx_, tools_obj, "check_syntax", JS_NewCFunction(ctx_, js_check_syntax, "check_syntax", 2));

  for (auto const& [name, handler] : dispatch_map) {
    if (name == "run_js") continue;

    auto tool_wrapper = [](JSContext* ctx, [[maybe_unused]] JSValueConst this_val, [[maybe_unused]] int argc,
                           JSValueConst* argv, [[maybe_unused]] int magic, JSValue* func_data) -> JSValue {
      ContextData* data = static_cast<ContextData*>(JS_GetContextOpaque(ctx));
      const char* name = JS_ToCString(ctx, func_data[0]);
      if (!name) return JS_EXCEPTION;
      absl::Cleanup free_name = [ctx, name] { JS_FreeCString(ctx, name); };
      nlohmann::json args = data->interpreter->JSToJSON(argv[0]);

      auto it = data->dispatch_map->find(name);
      if (it == data->dispatch_map->end()) {
        return JS_ThrowReferenceError(ctx, "Tool not found: %s", name);
      }

      auto result = it->second(args, data->cancellation);

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
