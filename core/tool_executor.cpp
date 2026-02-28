#include <fstream>
#include "js-bridge/interpreter.h"
#include "core/tool_executor.h"

#include <algorithm>
#include <memory>
#include <sstream>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#include "core/database.h"
#include "core/js_preamble_data.h"
#include "core/tool_dispatcher.h"
#include "json_utils.h"
#include "interface/terminal.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include <iostream>

namespace slop {

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
  auto it = dispatch_map_.find(name);
  if (it != dispatch_map_.end()) {
    auto res = it->second(args, cancellation);
    if (res.ok() && db_) {
      (void)db_->IncrementToolCallCount(name);
    }
    return res;
  }

    RunJsRequest req;
  req.script = "return core.dispatch_tool(args.name, args.tool_args)";
  req.args["name"] = name;
  req.args["tool_args"] = args;

  auto res = RunJs(req, cancellation);  if (!res.ok()) {
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
  slop::JsInterpreter interpreter;
  std::stringstream stdout_buffer;
  interpreter.InitializeEnvironment(db_, dispatcher_.get(), cancellation, dispatch_map_, stdout_buffer);

  JSContext* ctx = interpreter.context();
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global_obj, "session_id", JS_NewString(ctx, session_id_.c_str()));

  // Inject scratchpad
  auto scratchpad_res = db_->GetScratchpad(session_id_);
  if (scratchpad_res.ok() && !scratchpad_res->empty()) {
    JS_SetPropertyStr(ctx, global_obj, "scratchpad", JS_NewString(ctx, scratchpad_res->c_str()));
  }

  // Inject state
  auto state_res = db_->GetSessionState(session_id_);
  if (state_res.ok() && !state_res->empty()) {
    JS_SetPropertyStr(ctx, global_obj, "state", JS_NewString(ctx, state_res->c_str()));
  }

  // Inject history
  auto history_res = db_->GetConversationHistory(session_id_);
  if (history_res.ok() && !history_res->empty()) {
    JSValue history_array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& msg : *history_res) {
      JSValue msg_obj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, msg_obj, "role", JS_NewString(ctx, msg.role.c_str()));
      JS_SetPropertyStr(ctx, msg_obj, "content", JS_NewString(ctx, msg.content.c_str()));
      JS_SetPropertyUint32(ctx, history_array, i++, msg_obj);
    }
    JS_SetPropertyStr(ctx, global_obj, "history", history_array);
  }

  if (!req.args.is_null()) {
    JS_SetPropertyStr(ctx, global_obj, "args", interpreter.JSONToJS(req.args));
  }
  JS_FreeValue(ctx, global_obj);

  // Load persistent functions from the database
  if (db_) {
    auto functions_res = db_->Query("SELECT name, code FROM js_functions");
    if (functions_res.ok()) {
      if (auto functions_json = json_parse(*functions_res)) {
        for (const auto& row : *functions_json) {
          auto name = json_get<std::string>(row, "name");
          auto code = json_get<std::string>(row, "code");
          if (name && code) {
            // Wrap the code to return the function closure and bind it to globalThis
            std::string wrapped_code =
                "globalThis['" + *name + "'] = (function() {\n" + *code + "\n})();";
            JSValue func_res = interpreter.RunString(wrapped_code, "js_function_" + *name + ".js");
            JS_FreeValue(ctx, func_res);
          }
        }
      }
    }
  }

  // Load preamble
  JSValue preamble_res = interpreter.RunString(slop::kJsPreamble, "preamble.js");
  JS_FreeValue(ctx, preamble_res);


  std::string wrapped_script = "(function() {\n" + req.script + "\n})();";
  JSValue result = interpreter.RunString(wrapped_script, "input.js");
  
  JsResult res;
  res.stdout_out = stdout_buffer.str();
  if (JS_IsException(result)) {
      JSValue exception = JS_GetException(ctx);
      absl::Status status = absl::InternalError(absl::StrCat("JS Error\nOutput:\n", res.stdout_out));
      if (const char* str = JS_ToCString(ctx, exception)) {
          status = absl::InternalError(absl::StrCat(str, "\nOutput:\n", res.stdout_out));
          JS_FreeCString(ctx, str);
      }
      JS_FreeValue(ctx, exception);
      JS_FreeValue(ctx, result);
      return status;
  }
  
  if (!JS_IsUndefined(result)) {
      const char* str = JS_ToCString(ctx, result);
      if (str) {
          res.return_value = str;
          JS_FreeCString(ctx, str);
      }
  }
  JS_FreeValue(ctx, result);
  return res;
}

absl::StatusOr<std::string> ToolExecutor::HandleRunJs(const nlohmann::json& args,
                                                       std::shared_ptr<CancellationRequest> cancellation) {
  RunJsRequest req;
  auto script = json_get<std::string>(args, "script");
  if (!script) {
    return absl::InvalidArgumentError("'script' must be a string");
  }
  req.script = *script;
  if (const auto* js_args = json_at(args, "args")) req.args = *js_args;
  auto res = RunJs(req, cancellation);
  if (!res.ok()) return res.status();
  return res->FullOutput();
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
