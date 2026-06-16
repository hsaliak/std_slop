
#ifndef SLOP_JS_BRIDGE_INTERPRETER_H_
#define SLOP_JS_BRIDGE_INTERPRETER_H_

#include <functional>
#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "quickjs.h"

namespace slop {

class JsInterpreter {
 public:
  using ToolCaller = std::function<absl::StatusOr<std::string>(const std::string&, const nlohmann::json&)>;

  JsInterpreter();
  explicit JsInterpreter(ToolCaller tool_caller);
  JsInterpreter(const JsInterpreter&) = delete;
  JsInterpreter& operator=(const JsInterpreter&) = delete;
  ~JsInterpreter();

  absl::StatusOr<nlohmann::json> RunJson(std::string code, const std::string& filename = "<run_js>");
  absl::Status SetGlobalJson(const std::string& name, const nlohmann::json& value);

 private:
  struct ContextData;

  struct JsRuntimeDeleter {
    void operator()(JSRuntime* runtime) const;
  };

  struct JsContextDeleter {
    void operator()(JSContext* context) const;
  };

  absl::StatusOr<JSValue> Evaluate(std::string code, const std::string& filename);
  absl::StatusOr<nlohmann::json> ValueToJson(JSValueConst value);
  JSValue JsonToValue(const nlohmann::json& value);
  absl::Status InstallToolBridge();
  std::string ExceptionMessage();

  std::unique_ptr<JSRuntime, JsRuntimeDeleter> runtime_;
  std::unique_ptr<JSContext, JsContextDeleter> context_;
  ToolCaller tool_caller_;
  std::unique_ptr<ContextData> context_data_;
};

absl::StatusOr<nlohmann::json> RunJsForJson(std::string code, JsInterpreter::ToolCaller tool_caller = nullptr,
                                            const std::string& filename = "<run_js>");
absl::StatusOr<nlohmann::json> RunJsForJson(std::string code, const nlohmann::json& input,
                                            JsInterpreter::ToolCaller tool_caller = nullptr,
                                            const std::string& filename = "<run_js>");

absl::Status ValidateRunJsArgs(const nlohmann::json& args);
absl::StatusOr<nlohmann::json> ExecuteRunJsArgs(const nlohmann::json& args,
                                                JsInterpreter::ToolCaller tool_caller = nullptr);

}  // namespace slop

#endif  // SLOP_JS_BRIDGE_INTERPRETER_H_