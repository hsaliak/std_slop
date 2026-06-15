
#ifndef SLOP_JS_BRIDGE_INTERPRETER_H_
#define SLOP_JS_BRIDGE_INTERPRETER_H_

#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "quickjs.h"

namespace slop {

class JsInterpreter {
 public:
  JsInterpreter();
  JsInterpreter(const JsInterpreter&) = delete;
  JsInterpreter& operator=(const JsInterpreter&) = delete;
  ~JsInterpreter();

  absl::StatusOr<nlohmann::json> RunJson(std::string code, const std::string& filename = "<run_js>");

 private:
  struct JsRuntimeDeleter {
    void operator()(JSRuntime* runtime) const;
  };

  struct JsContextDeleter {
    void operator()(JSContext* context) const;
  };

  absl::StatusOr<JSValue> Evaluate(std::string code, const std::string& filename);
  absl::StatusOr<nlohmann::json> ValueToJson(JSValueConst value);
  std::string ExceptionMessage();

  std::unique_ptr<JSRuntime, JsRuntimeDeleter> runtime_;
  std::unique_ptr<JSContext, JsContextDeleter> context_;
};

absl::StatusOr<nlohmann::json> RunJsForJson(std::string code, const std::string& filename = "<run_js>");

absl::Status ValidateRunJsArgs(const nlohmann::json& args);
absl::StatusOr<nlohmann::json> ExecuteRunJsArgs(const nlohmann::json& args);

}  // namespace slop

#endif  // SLOP_JS_BRIDGE_INTERPRETER_H_