#include "js_runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "core/json_utils.h"
#include "js_runtime/value_conversion.h"
#include "quickjs.h"

namespace slop::js_runtime {
namespace {

constexpr std::size_t kMaxJsonInputBytes = 1024 * 1024;
constexpr std::size_t kMaxToolNameBytes = 256;
constexpr auto kPumpWait = std::chrono::milliseconds(10);

struct PendingCall {
  JSValue resolve;
  JSValue reject;
};

class Runtime {
 public:
  Runtime(AsyncToolBroker* broker, RuntimeOptions options, std::uint64_t run_id)
      : broker_(broker),
        options_(options),
        run_id_(run_id),
        deadline_(std::chrono::steady_clock::now() + options.timeout) {
    runtime_ = JS_NewRuntime();
    if (runtime_ == nullptr) return;
    JS_SetMemoryLimit(runtime_, options.memory_limit_bytes);
    JS_SetMaxStackSize(runtime_, 1024 * 1024);
    JS_SetInterruptHandler(runtime_, &Runtime::Interrupt, this);
    context_ = JS_NewContext(runtime_);
    if (context_ != nullptr) {
      JS_SetContextOpaque(context_, this);
    }
  }

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  ~Runtime() {
    if (context_ != nullptr) {
      for (const auto& [id, call] : pending_) {
        JS_FreeValue(context_, call.resolve);
        JS_FreeValue(context_, call.reject);
      }
      pending_.clear();
      JS_FreeValue(context_, prototypes_.object);
      JS_FreeValue(context_, prototypes_.array);
      JS_SetContextOpaque(context_, nullptr);
      JS_FreeContext(context_);
    }
    if (runtime_ != nullptr) JS_FreeRuntime(runtime_);
  }

  absl::StatusOr<nlohmann::json> RunCode(const std::string& code, const nlohmann::json& input) {
    if (runtime_ == nullptr || context_ == nullptr) return absl::InternalError("failed to initialize QuickJS");
    if (options_.timeout <= std::chrono::milliseconds::zero())
      return absl::InvalidArgumentError("timeout must be positive");
    absl::Status prototype_status = CapturePrototypes();
    if (!prototype_status.ok()) return prototype_status;

    const std::string input_text = json_dump(input);
    if (input_text.size() > kMaxJsonInputBytes) return absl::InvalidArgumentError("input exceeds maximum JSON size");
    JSValue input_value = JS_ParseJSON(context_, input_text.data(), input_text.size(), "<input>");
    if (JS_IsException(input_value)) return ExceptionStatus("input is not valid JSON");
    JSValue global = JS_GetGlobalObject(context_);
    if (JS_SetPropertyStr(context_, global, "input", input_value) < 0) {
      JS_FreeValue(context_, global);
      return ExceptionStatus("failed to install input");
    }
    JS_FreeValue(context_, global);

    absl::Status setup_status = InstallBridge();
    if (!setup_status.ok()) return setup_status;

    const std::string wrapped = absl::StrCat("(async function () {\n", code, "\n})()");
    JSValue root_promise = JS_Eval(context_, wrapped.data(), wrapped.size(), "<run_js>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(root_promise)) {
      if (Interrupt(runtime_, this) != 0)
        return absl::DeadlineExceededError("JavaScript execution exceeded its time limit");
      return ExceptionStatus("JavaScript evaluation failed");
    }
    absl::Cleanup free_promise = [this, root_promise] { JS_FreeValue(context_, root_promise); };
    if (!JS_IsPromise(root_promise)) return absl::InternalError("async wrapper did not return a Promise");

    absl::Status pump_status = Pump(root_promise);
    if (!pump_status.ok()) return pump_status;
    if (!pending_.empty()) {
      broker_->CancelPending();
      return absl::FailedPreconditionError("unawaited tool calls remain when the script completes");
    }
    auto result = PromiseResult(root_promise);
    return result;
  }

 private:
  static int Interrupt(JSRuntime*, void* opaque) {
    const auto* self = static_cast<const Runtime*>(opaque);
    return std::chrono::steady_clock::now() >= self->deadline_ ? 1 : 0;
  }

  static JSValue CallTool(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* self = static_cast<Runtime*>(JS_GetContextOpaque(context));
    if (self == nullptr) return JS_ThrowInternalError(context, "run_js runtime is not available");
    if (argc != 3) return JS_ThrowTypeError(context, "mcp.call expects server, tool, and arguments");
    if (!JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
      return JS_ThrowTypeError(context, "server and tool must be strings");
    }
    absl::StatusOr<std::string> server = ToString(context, argv[0]);
    absl::StatusOr<std::string> tool = ToString(context, argv[1]);
    if (!server.ok() || !tool.ok()) return JS_ThrowTypeError(context, "server and tool must be strings");
    if (server->empty() || tool->empty() || server->size() > kMaxToolNameBytes || tool->size() > kMaxToolNameBytes) {
      return JS_ThrowTypeError(context, "server and tool names must be non-empty and bounded");
    }
    if (!JS_IsObject(argv[2]) || JS_IsArray(argv[2])) {
      return JS_ThrowTypeError(context, "tool arguments must be an object");
    }
    if (self->admitted_calls_ >= self->options_.max_pending_calls) {
      return JS_ThrowInternalError(context, "tool call limit exceeded");
    }
    absl::StatusOr<nlohmann::json> arguments = self->ValueToJson(argv[2], kMaxJsonInputBytes);
    if (!arguments.ok() || !arguments->is_object()) {
      return JS_ThrowTypeError(context, "tool arguments must be a JSON object");
    }
    ++self->admitted_calls_;
    return self->Submit(*server, *tool, *arguments);
  }

  absl::Status CapturePrototypes() {
    JSValue global = JS_GetGlobalObject(context_);
    absl::Cleanup free_global = [this, global] { JS_FreeValue(context_, global); };
    JSValue object_constructor = JS_GetPropertyStr(context_, global, "Object");
    if (JS_IsException(object_constructor)) return ExceptionStatus("failed to read Object constructor");
    absl::Cleanup free_object = [this, object_constructor] { JS_FreeValue(context_, object_constructor); };
    JSValue array_constructor = JS_GetPropertyStr(context_, global, "Array");
    if (JS_IsException(array_constructor)) return ExceptionStatus("failed to read Array constructor");
    absl::Cleanup free_array = [this, array_constructor] { JS_FreeValue(context_, array_constructor); };
    prototypes_.object = JS_GetPropertyStr(context_, object_constructor, "prototype");
    prototypes_.array = JS_GetPropertyStr(context_, array_constructor, "prototype");
    if (JS_IsException(prototypes_.object) || JS_IsException(prototypes_.array)) {
      JS_FreeValue(context_, prototypes_.object);
      JS_FreeValue(context_, prototypes_.array);
      prototypes_.object = JS_UNDEFINED;
      prototypes_.array = JS_UNDEFINED;
      return ExceptionStatus("failed to read built-in prototypes");
    }
    return absl::OkStatus();
  }

  static JSValue Help(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* self = static_cast<Runtime*>(JS_GetContextOpaque(context));
    if (self == nullptr) return JS_ThrowInternalError(context, "run_js runtime is not available");
    if (argc > 2) return JS_ThrowTypeError(context, "help accepts at most server and tool names");
    std::string server;
    std::string tool;
    if (argc > 0) {
      if (!JS_IsString(argv[0])) return JS_ThrowTypeError(context, "help server must be a string");
      absl::StatusOr<std::string> value = ToString(context, argv[0]);
      if (!value.ok()) return JS_ThrowTypeError(context, "help server must be a string");
      server = std::move(*value);
    }
    if (argc > 1) {
      if (!JS_IsString(argv[1])) return JS_ThrowTypeError(context, "help tool must be a string");
      absl::StatusOr<std::string> value = ToString(context, argv[1]);
      if (!value.ok()) return JS_ThrowTypeError(context, "help tool must be a string");
      tool = std::move(*value);
    }
    absl::StatusOr<nlohmann::json> help = self->broker_->Help(server, tool);
    if (!help.ok()) {
      const std::string message(help.status().message());
      return JS_ThrowTypeError(context, "%s", message.c_str());
    }
    return self->JsonToValue(*help);
  }

  static absl::StatusOr<std::string> ToString(JSContext* context, JSValueConst value) {
    size_t length = 0;
    const char* raw = JS_ToCStringLen(context, &length, value);
    if (raw == nullptr) return absl::InvalidArgumentError("value is not a string");
    absl::Cleanup free_raw = [context, raw] { JS_FreeCString(context, raw); };
    return std::string(raw, length);
  }

  absl::Status InstallBridge() {
    JSValue global = JS_GetGlobalObject(context_);
    absl::Cleanup free_global = [this, global] { JS_FreeValue(context_, global); };
    JSValue mcp = JS_NewObjectProto(context_, JS_NULL);
    if (JS_IsException(mcp)) return ExceptionStatus("failed to create mcp object");
    JSValue call = JS_NewCFunction(context_, &Runtime::CallTool, "call", 3);
    if (JS_IsException(call) || JS_SetPropertyStr(context_, mcp, "call", call) < 0) {
      JS_FreeValue(context_, mcp);
      return ExceptionStatus("failed to install mcp.call");
    }
    JSValue help = JS_NewCFunction(context_, &Runtime::Help, "help", 2);
    if (JS_IsException(help) || JS_SetPropertyStr(context_, mcp, "help", help) < 0) {
      JS_FreeValue(context_, mcp);
      return ExceptionStatus("failed to install mcp.help");
    }
    if (JS_SetPropertyStr(context_, global, "mcp", mcp) < 0) return ExceptionStatus("failed to install mcp");
    JSValue global_help = JS_NewCFunction(context_, &Runtime::Help, "help", 2);
    if (JS_IsException(global_help) || JS_SetPropertyStr(context_, global, "help", global_help) < 0) {
      return ExceptionStatus("failed to install help");
    }
    JSValue catalog = JsonToValue(broker_->PublicCatalog());
    if (JS_IsException(catalog) || JS_SetPropertyStr(context_, global, "__slop_catalog", catalog) < 0) {
      return ExceptionStatus("failed to install tool catalog");
    }
    static constexpr char kInstallProxies[] =
        "(() => { const call = globalThis.mcp.call; "
        "for (const server of globalThis.__slop_catalog) { "
        "if (server.alias in globalThis) throw new TypeError('server alias collides with a global'); "
        "const proxy = Object.create(null); "
        "for (const tool of server.tools) { "
        "Object.defineProperty(proxy, tool.name, {enumerable: true, value: (args = {}) => call(server.alias, "
        "tool.name, args)}); } "
        "Object.defineProperty(globalThis, server.alias, {configurable: false, enumerable: true, writable: false, "
        "value: proxy}); } "
        "delete globalThis.__slop_catalog; })()";
    JSValue installed =
        JS_Eval(context_, kInstallProxies, sizeof(kInstallProxies) - 1, "<mcp_bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(installed)) return ExceptionStatus("failed to install tool proxies");
    JS_FreeValue(context_, installed);
    return absl::OkStatus();
  }

  JSValue Submit(const std::string& server, const std::string& tool, const nlohmann::json& arguments) {
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context_, resolving);
    if (JS_IsException(promise)) return promise;
    const std::uint64_t id = next_id_++;
    const ToolRequest request{run_id_, id, deadline_, server, tool, arguments};
    absl::Status status = broker_->Submit(request);
    if (!status.ok()) {
      const nlohmann::json error_data = {{"category", "admission"}, {"message", status.message()}};
      JSValue error = JsonToValue(error_data);
      JSValue rejected = JS_Call(context_, resolving[1], JS_UNDEFINED, 1, &error);
      JS_FreeValue(context_, error);
      JS_FreeValue(context_, rejected);
      JS_FreeValue(context_, resolving[0]);
      JS_FreeValue(context_, resolving[1]);
      return promise;
    }
    pending_.emplace(id, PendingCall{resolving[0], resolving[1]});
    return promise;
  }

  absl::Status Pump(JSValueConst root_promise) {
    while (JS_PromiseState(context_, root_promise) == JS_PROMISE_PENDING) {
      if (Interrupt(runtime_, this) != 0) {
        broker_->CancelPending();
        return absl::DeadlineExceededError("JavaScript execution exceeded its time limit");
      }
      bool progressed = CompleteReadyCalls();
      if (!pending_.empty()) {
        absl::StatusOr<bool> jobs = RunPendingJobs();
        if (!jobs.ok()) {
          broker_->CancelPending();
          return jobs.status();
        }
        progressed = *jobs || progressed;
        if (JS_PromiseState(context_, root_promise) != JS_PROMISE_PENDING) break;
        if (!progressed) {
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now());
          broker_->WaitForCompletion(std::min(kPumpWait, std::max(remaining, std::chrono::milliseconds::zero())));
        }
      } else {
        absl::StatusOr<bool> jobs = RunPendingJobs();
        if (!jobs.ok()) {
          broker_->CancelPending();
          return jobs.status();
        }
        progressed = *jobs || progressed;
        if (JS_PromiseState(context_, root_promise) == JS_PROMISE_PENDING && !progressed) {
          return absl::FailedPreconditionError("stalled_promise: script is pending without native work");
        }
      }
    }
    if (JS_PromiseState(context_, root_promise) == JS_PROMISE_REJECTED) {
      if (Interrupt(runtime_, this) != 0)
        return absl::DeadlineExceededError("JavaScript execution exceeded its time limit");
      if (!pending_.empty()) broker_->CancelPending();
      return PromiseError(root_promise);
    }
    return absl::OkStatus();
  }

  bool CompleteReadyCalls() {
    bool progressed = false;
    for (const ToolCompletion& completion : broker_->TakeCompletions()) {
      if (completion.run_id != run_id_) continue;
      auto it = pending_.find(completion.id);
      if (it == pending_.end()) continue;
      JSValue value;
      JSValueConst function;
      if (completion.ok) {
        value = JsonToValue(completion.result);
        function = it->second.resolve;
      } else {
        value = JsonToValue({{"category", completion.error_category}, {"message", completion.error}});
        function = it->second.reject;
      }
      if (!JS_IsException(value)) {
        JSValue returned = JS_Call(context_, function, JS_UNDEFINED, 1, &value);
        JS_FreeValue(context_, returned);
      }
      JS_FreeValue(context_, value);
      JS_FreeValue(context_, it->second.resolve);
      JS_FreeValue(context_, it->second.reject);
      pending_.erase(it);
      progressed = true;
    }
    return progressed;
  }

  absl::StatusOr<bool> RunPendingJobs() {
    bool progressed = false;
    for (std::size_t i = 0; i < options_.max_jobs_per_turn; ++i) {
      if (Interrupt(runtime_, this) != 0) return progressed;
      JSContext* job_context = nullptr;
      const int result = JS_ExecutePendingJob(runtime_, &job_context);
      if (result == 0) return progressed;
      if (result < 0) {
        JSValue exception = JS_GetException(job_context);
        absl::StatusOr<std::string> message = SafeErrorMessage(exception);
        JS_FreeValue(job_context, exception);
        return absl::InvalidArgumentError(
            absl::StrCat("JavaScript job failed: ", message.ok() ? *message : "unknown error"));
      }
      progressed = true;
    }
    return progressed;
  }

  absl::StatusOr<nlohmann::json> PromiseResult(JSValueConst promise) {
    JSValue value = JS_PromiseResult(context_, promise);
    if (JS_IsException(value)) return ExceptionStatus("failed to read promise result");
    absl::Cleanup free_value = [this, value] { JS_FreeValue(context_, value); };
    return ValueToJson(value, options_.max_output_bytes);
  }

  absl::Status PromiseError(JSValueConst promise) {
    JSValue reason = JS_PromiseResult(context_, promise);
    if (JS_IsException(reason)) return ExceptionStatus("JavaScript promise rejected");
    absl::Cleanup free_reason = [this, reason] { JS_FreeValue(context_, reason); };
    if (JS_IsObject(reason)) {
      absl::StatusOr<nlohmann::json> error = ValueToJson(reason, 4096);
      if (error.ok() && error->is_object()) {
        const std::string category = json_get_or(*error, "category", std::string("javascript"));
        const std::string message = json_get_or(*error, "message", std::string("unknown error"));
        return absl::InvalidArgumentError(absl::StrCat("JavaScript promise rejected (", category, "): ", message));
      }
    }
    if (JS_IsObject(reason)) {
      return absl::InvalidArgumentError("JavaScript promise rejected with a non-JSON object");
    }
    absl::StatusOr<std::string> message = SafeErrorMessage(reason);
    return absl::InvalidArgumentError(
        absl::StrCat("JavaScript promise rejected: ", message.ok() ? *message : "unknown error"));
  }

  absl::StatusOr<nlohmann::json> ValueToJson(JSValueConst value, std::size_t max_bytes) {
    absl::StatusOr<nlohmann::json> converted = JsonFromJs(context_, value, prototypes_, max_bytes);
    if (!converted.ok()) return converted.status();
    const std::string serialized = json_dump(*converted);
    if (serialized.size() > max_bytes) return absl::ResourceExhaustedError("JSON value exceeds size limit");
    return converted;
  }

  JSValue JsonToValue(const nlohmann::json& value) {
    const std::string serialized = json_dump(value);
    return JS_ParseJSON(context_, serialized.data(), serialized.size(), "<tool-result>");
  }

  absl::StatusOr<std::string> SafeErrorMessage(JSValueConst value) {
    if (JS_IsObject(value)) {
      absl::StatusOr<nlohmann::json> error = ValueToJson(value, 4096);
      if (error.ok() && error->is_object()) {
        const auto message = json_get<std::string>(*error, "message");
        if (message.has_value()) return *message;
      }
      return absl::InvalidArgumentError("non-JSON object error");
    }
    if (JS_IsString(value) || JS_IsNumber(value) || JS_IsBool(value) || JS_IsNull(value) || JS_IsUndefined(value) ||
        JS_IsBigInt(value)) {
      return ToString(context_, value);
    }
    return absl::InvalidArgumentError("non-string JavaScript error");
  }

  absl::Status ExceptionStatus(const char* prefix) {
    JSValue exception = JS_GetException(context_);
    absl::Cleanup free_exception = [this, exception] { JS_FreeValue(context_, exception); };
    absl::StatusOr<std::string> message = SafeErrorMessage(exception);
    return absl::InvalidArgumentError(absl::StrCat(prefix, ": ", message.ok() ? *message : "unknown JavaScript error"));
  }

  AsyncToolBroker* broker_;
  RuntimeOptions options_;
  std::uint64_t run_id_;
  std::chrono::steady_clock::time_point deadline_;
  JSRuntime* runtime_ = nullptr;
  JSContext* context_ = nullptr;
  std::uint64_t next_id_ = 1;
  std::size_t admitted_calls_ = 0;
  std::map<std::uint64_t, PendingCall> pending_;
  JsPrototypes prototypes_{JS_UNDEFINED, JS_UNDEFINED};
};

}  // namespace

absl::StatusOr<nlohmann::json> Run(std::string code, const nlohmann::json& input, AsyncToolBroker* broker,
                                   RuntimeOptions options) {
  if (broker == nullptr) return absl::InvalidArgumentError("tool broker must not be null");
  if (code.size() > options.max_code_bytes) return absl::InvalidArgumentError("code exceeds maximum size");
  if (code.find('\0') != std::string::npos) return absl::InvalidArgumentError("code contains a NUL byte");
  if (options.max_pending_calls == 0 || options.max_jobs_per_turn == 0 || options.memory_limit_bytes == 0) {
    return absl::InvalidArgumentError("runtime limits must be positive");
  }
  static std::atomic<std::uint64_t> next_run_id = 1;
  std::uint64_t run_id = next_run_id.load();
  while (run_id != std::numeric_limits<std::uint64_t>::max() &&
         !next_run_id.compare_exchange_weak(run_id, run_id + 1)) {
  }
  if (run_id == std::numeric_limits<std::uint64_t>::max()) {
    return absl::ResourceExhaustedError("run ID space is exhausted");
  }
  Runtime runtime(broker, options, run_id);
  return runtime.RunCode(code, input);
}

}  // namespace slop::js_runtime
