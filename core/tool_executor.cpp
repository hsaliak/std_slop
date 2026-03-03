#include <fstream>
#include "js-bridge/interpreter.h"
#include "core/tool_executor.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/database.h"
#include "core/js_preamble_data.h"
#include "core/tool_dispatcher.h"
#include "json_utils.h"
#include "interface/terminal.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include <iostream>

namespace slop {
namespace {

bool IsDebugToolsEnabled() {
  return std::getenv("SLOP_DEBUG_TOOLS") != nullptr;
}

std::string JsonKeys(const nlohmann::json& j) {
  if (!j.is_object()) return "<non-object>";
  std::vector<std::string> keys;
  keys.reserve(j.size());
  for (auto it = j.begin(); it != j.end(); ++it) {
    keys.push_back(it.key());
  }
  return keys.empty() ? "<empty>" : absl::StrJoin(keys, ",");
}

std::string TruncateForLog(const std::string& s, size_t max_len = 240) {
  if (s.size() <= max_len) return s;
  return s.substr(0, max_len) + "...";
}

}  // namespace

ToolExecutor::ToolExecutor(Database* db) : db_(db) { RegisterTools(); }

ToolExecutor::~ToolExecutor() = default;

void ToolExecutor::SetDispatcher(std::unique_ptr<ToolDispatcher> dispatcher) { dispatcher_ = std::move(dispatcher); }

void ToolExecutor::RegisterTool(const std::string& name, ToolHandler handler) {
  dispatch_map_[name] = std::move(handler);
}

absl::StatusOr<std::string> ToolExecutor::HandleQueryDb(const nlohmann::json& args) {
  if (!db_) {
    return absl::FailedPreconditionError("Database not initialized");
  }
  if (!args.is_object()) {
    return absl::InvalidArgumentError("Arguments must be a JSON object");
  }

  auto sql = json_get<std::string>(args, "sql");
  if (!sql) {
    return absl::InvalidArgumentError("'sql' must be a string");
  }

  std::vector<std::string> params;
  if (auto p_array = json_get<nlohmann::json::array_t>(args, "params")) {
    for (const auto& p : *p_array) {
      if (p.is_string()) {
        params.push_back(p.get<std::string>());
      } else if (p.is_null()) {
        params.emplace_back("NULL");
      } else {
        // For numbers, booleans, objects, and arrays, stringify them.
        params.push_back(p.dump());
      }
    }
  }
  return db_->Query(*sql, params);
}


void ToolExecutor::RegisterTools() {
  RegisterTool("query_db", [this](const nlohmann::json& args, auto) { return HandleQueryDb(args); });

  RegisterTool("run_js", [this](const nlohmann::json& args, std::shared_ptr<CancellationRequest> cancellation) {
    return HandleRunJs(args, cancellation);
  });
    RegisterTool("ask_user", [this](const nlohmann::json& args, auto) -> absl::StatusOr<std::string> {
    std::string prompt_text = "Input required: ";
    if (auto p = json_get<std::string>(args, "prompt")) {
      prompt_text = *p;
    }

    while (true) {
      std::string response;
      if (ask_user_handler_) {
        response = ask_user_handler_(prompt_text);
      } else {
        std::cout << "\n" << ansi::Yellow << "Agent asks:\n" << ansi::Reset;
        slop::Renderer::Get().PrintMarkdown(prompt_text);
        response = slop::ReadLine("reply");
      }

      if (!absl::StartsWith(response, "/")) {
        return response;
      }

      std::cout << "\n" << ansi::Red << "Error: " << ansi::Reset
                << "/commands don't work in Q&A mode. Please provide a direct answer without using slash commands."
                << std::endl;
    }
  });
}

absl::StatusOr<std::string> ToolExecutor::Execute(const std::string& name, const nlohmann::json& args,
                                                  std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] Execute name=" << name << " args_keys=" << JsonKeys(args);
  }
  auto it = dispatch_map_.find(name);
  if (it != dispatch_map_.end()) {
    auto res = it->second(args, cancellation);
    if (IsDebugToolsEnabled()) {
      LOG(INFO) << "[tool_debug] Execute direct name=" << name << " status=" << (res.ok() ? "ok" : "error")
                << " output_preview=" << (res.ok() ? TruncateForLog(*res) : TruncateForLog(res.status().ToString()));
    }
    if (res.ok() && db_) {
      (void)db_->IncrementToolCallCount(name);
    }
    return res;
  }

    RunJsRequest req;
  req.script = "return core.dispatch_tool(args.name, args.tool_args)";
  req.args["name"] = name;
  req.args["tool_args"] = args;

  auto res = RunJs(req, cancellation);
  if (!res.ok()) {
    if (IsDebugToolsEnabled()) {
      LOG(INFO) << "[tool_debug] Execute via run_js failed name=" << name << " status=" << res.status();
    }
    std::string msg = std::string(res.status().message());
    if (absl::StrContains(msg, "NOT_FOUND:")) {
      return absl::NotFoundError(msg);
    }
    if (absl::StrContains(msg, "FAILED_PRECONDITION:")) {
      return absl::FailedPreconditionError(msg);
    }
    if (absl::StrContains(msg, "INVALID_ARGUMENT:")) {
      return absl::InvalidArgumentError(msg);
    }
    return res.status();
  }

  if (db_) {
    (void)db_->IncrementToolCallCount(name);
  }
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] Execute via run_js success name=" << name
              << " return_preview=" << TruncateForLog(res->return_value);
  }
  auto envelope = nlohmann::json::parse(res->return_value, nullptr, false);
  if (!envelope.is_discarded() && envelope.is_object() && envelope.contains("ok")) {
    if (!envelope["ok"].get<bool>()) {
      std::string msg = "Tool error";
      if (envelope.contains("error") && envelope["error"].is_object() && envelope["error"].contains("message")) {
        msg = envelope["error"]["message"].get<std::string>();
      }
      if (absl::StrContains(msg, "Destructive operations are only allowed")) {
        return absl::FailedPreconditionError(msg);
      }
    }
  }
  return res->return_value;
}

void ToolExecutor::SetSessionId(const std::string& session_id) { session_id_ = session_id; }

void ToolExecutor::SetMailMode(bool enabled) {
  mail_mode_ = enabled;
  if (db_) {
    (void)db_->Query(enabled ? "UPDATE settings SET mode = 'mail' WHERE id = 1"
                             : "UPDATE settings SET mode = 'standard' WHERE id = 1");
  }
}

bool ToolExecutor::IsSkillActive(const std::string& name) {
  auto active = GetActiveSkills();
  return std::any_of(active.begin(), active.end(), [&name](const std::string& s) { return s == name; });
}

std::vector<std::string> ToolExecutor::GetActiveSkills() {
  if (session_id_.empty() || !db_) return {};
  auto skills_or = db_->GetActiveSkills(session_id_);
  if (skills_or.ok()) {
    return *skills_or;
  }
  return {};
}

absl::StatusOr<ToolExecutor::JsResult> ToolExecutor::RunJs(const RunJsRequest& req,
                                                             std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] RunJs begin script_len=" << req.script.size()
              << " script_preview=" << TruncateForLog(req.script)
              << " has_args=" << (!req.args.is_null());
    if (!req.args.is_null()) {
      LOG(INFO) << "[tool_debug] RunJs args_keys=" << JsonKeys(req.args);
    }
  }
  slop::JsInterpreter interpreter;
  std::stringstream stdout_buffer;
  interpreter.InitializeEnvironment(db_, dispatcher_.get(), cancellation, dispatch_map_, stdout_buffer);

  JSContext* ctx = interpreter.context();
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global_obj, "session_id", JS_NewString(ctx, session_id_.c_str()));

  // Inject state
  auto state_res = db_->GetSessionState(session_id_);
  if (state_res.ok() && !state_res->empty()) {
    JS_SetPropertyStr(ctx, global_obj, "state", JS_NewString(ctx, state_res->c_str()));
  }

  if (!req.args.is_null()) {
    JS_SetPropertyStr(ctx, global_obj, "args", interpreter.JSONToJS(req.args));
  }
  JS_FreeValue(ctx, global_obj);

  // Load persistent functions from the database
  if (db_) {
    auto functions_res = db_->Query("SELECT name, code, json_schema FROM js_functions");
    if (functions_res.ok()) {
      if (auto functions_json = json_parse(*functions_res)) {
        if (auto rows = json_getter<std::vector<nlohmann::json>>::get(*functions_json)) {
          for (const auto& row : *rows) {
            auto name = json_get<std::string>(row, "name");
            auto code = json_get<std::string>(row, "code");
            auto json_schema = json_get<std::string>(row, "json_schema");
            if (name && code) {
              std::string target = (json_schema && !json_schema->empty()) ? "tools" : "globalThis";
              std::string wrapped_code = target + "['" + *name + "'] = (function() {\n" + *code + "\n})();";
              JSValue func_res = interpreter.RunString(wrapped_code, "js_function_" + *name + ".js", false);
              JS_FreeValue(ctx, func_res);
            }
          }
        }
      }
    }
  }

  // Load preamble
  JSValue preamble_res = interpreter.RunString(slop::kJsPreamble, "preamble.js", false);
  JS_FreeValue(ctx, preamble_res);


  JSValue result = interpreter.RunString(req.script, "input.js");
  
  JsResult res;
  res.stdout_out = stdout_buffer.str();
  bool had_js_return_value = false;
  if (JS_IsException(result)) {
      JSValue exception = JS_GetException(ctx);
      absl::Status status = absl::InternalError(absl::StrCat("JS Error\nOutput:\n", res.stdout_out));
      if (const char* str = JS_ToCString(ctx, exception)) {
          status = absl::InternalError(absl::StrCat(str, "\nOutput:\n", res.stdout_out));
          JS_FreeCString(ctx, str);
      }
      JS_FreeValue(ctx, exception);
      JS_FreeValue(ctx, result);
      if (IsDebugToolsEnabled()) {
        LOG(INFO) << "[tool_debug] RunJs exception status=" << status;
      }
      return status;
  }
  
  if (!JS_IsUndefined(result)) {
      had_js_return_value = true;
      JSValue printable = result;
      bool owns_printable = false;
      if (JS_IsObject(result)) {
        JSValue json_value = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json_value)) {
          printable = json_value;
          owns_printable = true;
        } else {
          // Clear stringify exception and fall back to default JS string coercion.
          JSValue exception = JS_GetException(ctx);
          JS_FreeValue(ctx, exception);
        }
      }
      const char* str = JS_ToCString(ctx, printable);
      if (str) {
          res.return_value = str;
          JS_FreeCString(ctx, str);
      }
      if (owns_printable) {
        JS_FreeValue(ctx, printable);
      }
  }
  JS_FreeValue(ctx, result);
  res.has_js_return_value = had_js_return_value;
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] RunJs success stdout_len=" << res.stdout_out.size()
              << " return_len=" << res.return_value.size()
              << " return_preview=" << TruncateForLog(res.return_value);
    if (!had_js_return_value) {
      LOG(INFO) << "[tool_debug] RunJs JS result was undefined. Script likely executed without an explicit return.";
    }
  }
  return res;
}

absl::StatusOr<std::string> ToolExecutor::HandleRunJs(const nlohmann::json& args,
                                                       std::shared_ptr<CancellationRequest> cancellation) {
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] HandleRunJs args_keys=" << JsonKeys(args);
  }
  RunJsRequest req;
  auto script = json_get<std::string>(args, "script");
  const auto* nested_args = json_at(args, "args");
  if (!script && nested_args != nullptr && nested_args->is_object()) {
    script = json_get<std::string>(*nested_args, "script");
  }
  if (!script) {
    script = json_get<std::string>(args, "code");
  }
  if (!script) {
    script = json_get<std::string>(args, "javascript");
  }
  if (!script) {
    std::string arg_shape = args.is_object() ? json_dump(args) : std::string("<non-object>");
    if (arg_shape.size() > 512) {
      arg_shape = arg_shape.substr(0, 512) + "...";
    }
    return absl::InvalidArgumentError(
        absl::StrCat("'script' must be a string (also accepted: args.script, code, javascript). Received: ",
                     arg_shape));
  }
  req.script = *script;
  if (nested_args != nullptr) {
    req.args = *nested_args;
  }
  if (IsDebugToolsEnabled()) {
    LOG(INFO) << "[tool_debug] HandleRunJs resolved script_source="
              << (json_get<std::string>(args, "script") ? "script"
                  : (nested_args != nullptr && json_get<std::string>(*nested_args, "script") ? "args.script"
                     : (json_get<std::string>(args, "code") ? "code" : "javascript")))
              << " script_len=" << req.script.size();
  }
  auto res = RunJs(req, cancellation);
  if (!res.ok()) return res.status();
  std::string output = res->FullOutput();
  if (output.empty()) {
    return absl::FailedPreconditionError("run_js produced no output: script must return a value or print output");
  }
  return output;
}

absl::StatusOr<std::string> ToolExecutor::GetBaseBranch(const std::string& requested_base) {
  RunJsRequest req;
  req.script = "return git.get_base_branch(args.requested_base)";
  req.args["requested_base"] = requested_base;
  auto res = RunJs(req, nullptr);
  if (!res.ok()) return res.status();
  return res->return_value;
}

}  // namespace slop













