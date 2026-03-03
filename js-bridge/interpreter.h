#pragma once

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "quickjs.h"

namespace slop {

class ToolDispatcher;
class CancellationRequest;
class Database;

class JsInterpreter {
 public:
  JsInterpreter();
  ~JsInterpreter();

  // Run a JS script from a string.
  JSValue RunString(const std::string& code, const std::string& filename = "input.js", bool wrap = true);

  // Run a JS script from a file.
  JSValue RunFile(const std::string& path);

  // Access the underlying JSContext
  JSContext* context() const { return ctx_; }

  // Initialize the environment with tools and globals
  void InitializeEnvironment(
      Database* db, ToolDispatcher* dispatcher, std::shared_ptr<CancellationRequest> cancellation,
      const absl::flat_hash_map<std::string, std::function<absl::StatusOr<std::string>(
                                                 const nlohmann::json&, std::shared_ptr<CancellationRequest>)>>&
          dispatch_map,
      std::stringstream& stdout_buffer);

  // Helpers for JSON conversion
  JSValue JSONToJS(const nlohmann::json& j);
  nlohmann::json JSToJSON(JSValue val);

 private:
  JSRuntime* rt_;
  JSContext* ctx_;
};

}  // namespace slop
