#include "interface/interaction_engine.h"

#include <atomic>
#include <iostream>
#include <thread>

#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "core/cancellation.h"
#include "core/constants.h"
#include "core/shell_util.h"
#include "interface/color.h"
#include "interface/ui.h"

namespace slop {

InteractionEngine::InteractionEngine(Database& db, Orchestrator& orchestrator,
                                     CommandHandler& cmd_handler, ToolDispatcher& dispatcher,
                                     ToolExecutor& tool_executor, HttpClient& http_client,
                                     std::shared_ptr<OAuthHandler> oauth_handler)
    : db_(db),
      orchestrator_(orchestrator),
      cmd_handler_(cmd_handler),
      dispatcher_(dispatcher),
      tool_executor_(tool_executor),
      http_client_(http_client),
      oauth_handler_(oauth_handler) {}

bool InteractionEngine::Process(std::string& input, std::string& session_id,
                                 std::vector<std::string>& active_skills, const Config& config) {
  if (input.empty()) return true;

  if (input == "/exit" || input == "/quit") return false;

  if (!config.is_batch_mode) {
    std::string echo = input;
    if (echo.length() > 60) {
      echo = echo.substr(0, 57) + "...";
    }
    std::cout << " " << slop::Colorize(" > " + echo + " ", ansi::EchoBg, ansi::EchoFg) << "\n"
              << std::endl;
  }

  auto res = cmd_handler_.Handle(input, session_id, active_skills, []() { ShowHelp(); },
                                 orchestrator_.GetLastSelectedGroups());
  if (res == CommandHandler::Result::HANDLED || res == CommandHandler::Result::UNKNOWN) {
    return true;
  }

  tool_executor_.SetSessionId(session_id);

  // Execute interaction
  std::string group_id = std::to_string(absl::ToUnixNanos(absl::Now()));
  (void)db_.AppendMessage(session_id, "user", input, "", "completed", group_id, orchestrator_.GetName());

  while (true) {
    auto prompt_or = orchestrator_.AssemblePrompt(session_id, active_skills);
    if (!prompt_or.ok()) {
      slop::HandleStatus(prompt_or.status(), "Prompt Error");
      break;
    }

    std::vector<std::string> headers = {"Content-Type: application/json"};
    std::string url;

    if (orchestrator_.GetProvider() == slop::Orchestrator::Provider::OPENAI) {
      headers.push_back("Authorization: Bearer " + config.openai_api_key);
      url = (!config.openai_base_url.empty() ? config.openai_base_url : slop::kOpenAIBaseUrl) +
            "/chat/completions";
    } else if (config.google_oauth && oauth_handler_) {
      auto token_or = oauth_handler_->GetValidToken();
      if (token_or.ok()) headers.push_back("Authorization: Bearer " + *token_or);
      url = absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal:generateContent");
    } else {
      headers.push_back("x-goog-api-key: " + config.google_api_key);
      url = absl::StrCat(slop::kPublicGeminiBaseUrl, "/models/", orchestrator_.GetModel(),
                         ":generateContent?key=", config.google_api_key);
    }

    auto resp_or = http_client_.Post(
        url, prompt_or->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), headers);
    if (!resp_or.ok()) {
      if (resp_or.status().code() == absl::StatusCode::kInvalidArgument) {
        LOG(WARNING) << "HTTP 400 error detected. Attempting to auto-fix history...";
        auto history_or = db_.GetConversationHistory(session_id, false, 10);
        if (history_or.ok() && !history_or->empty()) {
          bool dropped = false;
          for (auto it = history_or->rbegin(); it != history_or->rend(); ++it) {
            if (it->status == "tool_call" || it->role == "tool") {
              LOG(INFO) << "Dropping message " << it->id << " to fix 400 error.";
              (void)db_.UpdateMessageStatus(it->id, "dropped");
              dropped = true;
              break;
            }
          }
          if (dropped) {
            (void)db_.AppendMessage(session_id, "user",
                                   "History auto-fixed by dropping problematic tool calls.");
            continue;
          }
        }
      }

      if (!resp_or.ok()) {
        slop::HandleStatus(resp_or.status(), "HTTP Error");
        if (config.google_oauth && oauth_handler_ &&
            (absl::IsUnauthenticated(resp_or.status()) || absl::IsPermissionDenied(resp_or.status()))) {
          std::cout << "Refreshing OAuth token..." << std::endl;
          (void)oauth_handler_->GetValidToken();
        }
        break;
      }
    }

    auto history_before_or = db_.GetMessagesByGroups({group_id});
    size_t start_idx = history_before_or.ok() ? history_before_or->size() : 0;

    auto process_or = orchestrator_.ProcessResponse(session_id, *resp_or, group_id);
    if (!process_or.ok()) {
      slop::HandleStatus(process_or.status(), "Process Error");
      break;
    }
    (void)*process_or;

    auto history_after_or = db_.GetMessagesByGroups({group_id});
    if (!history_after_or.ok() || history_after_or->empty()) break;

    bool has_tool_calls = false;
    for (size_t i = start_idx; i < history_after_or->size(); ++i) {
      const auto& msg = (*history_after_or)[i];
      slop::PrintMessage(msg);

      if (msg.role == "assistant") {
        auto calls_or = orchestrator_.ParseToolCalls(msg);
        if (calls_or.ok() && !calls_or->empty()) {
          std::vector<slop::ToolDispatcher::Call> dispatcher_calls;
          for (const auto& call : *calls_or) {
            std::string combined_id = call.id;
            if (call.id != call.name && !absl::StrContains(call.id, '|')) {
              combined_id = call.id + "|" + call.name;
            }
            dispatcher_calls.push_back({combined_id, call.name, call.args});
          }

          auto cancellation = std::make_shared<slop::CancellationRequest>();
          std::atomic<bool> done{false};
          std::vector<slop::ToolDispatcher::Result> results;

          std::thread t([&] {
            results = dispatcher_.Dispatch(dispatcher_calls, cancellation);
            done = true;
          });

          while (!done) {
            if (slop::IsEscPressed()) {
              cancellation->Cancel();
              std::cerr << "\n"
                        << "  " << slop::Colorize("[Esc] Cancellation requested...", "", "\033[31m")
                        << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          t.join();

          for (const auto& res : results) {
            std::string result_content =
                res.output.ok() ? *res.output : absl::StrCat("Error: ", res.output.status().message());
            slop::PrintToolResultMessage(res.name, result_content, res.output.ok() ? "completed" : "error", "  ");
            (void)db_.AppendMessage(session_id, "tool", result_content, res.id,
                                   res.output.ok() ? "completed" : "error", group_id,
                                   msg.parsing_strategy);
          }
          has_tool_calls = true;
        }
      }
    }

    if (has_tool_calls) {
      if (orchestrator_.GetThrottle() > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(orchestrator_.GetThrottle()));
      }
      continue;  // Loop for next LLM turn
    }
    break;
  }

  return true;
}

}  // namespace slop
