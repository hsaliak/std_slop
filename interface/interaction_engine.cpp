#include "interface/interaction_engine.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "core/cancellation.h"
#include "core/constants.h"
#include "core/shell_util.h"
#include "core/status_macros.h"
#include "core/json_utils.h"
#include "core/responses_event_decoder.h"
#include "interface/color.h"
#include "interface/renderer.h"
#include "interface/terminal.h"
#include "interface/ui.h"
namespace slop {
namespace {

}  // namespace

InteractionEngine::InteractionEngine(Database& db, Orchestrator& orchestrator, CommandHandler& cmd_handler,
                                     ToolDispatcher& dispatcher, ToolExecutor& tool_executor, HttpClient& http_client,
                                     std::shared_ptr<OAuthHandler> oauth_handler)
    : db_(db),
      orchestrator_(orchestrator),
      cmd_handler_(cmd_handler),
      dispatcher_(dispatcher),
      tool_executor_(tool_executor),
      http_client_(http_client),
      oauth_handler_(oauth_handler) {}
bool InteractionEngine::Process(std::string& input, std::string& session_id, std::vector<std::string>& active_skills,
                                const Config& config) {
  bool is_hey_mode = false;
  std::string hey_skill_name;
  const std::vector<std::string> original_active_skills = active_skills;
  // RAII guard to restore active skills if we're in "hey" mode.
  struct HeyModeGuard {
    bool& active;
    std::vector<std::string>& current;
    const std::vector<std::string> original;
    const std::string& session;
    Database& database;
    ~HeyModeGuard() {
      if (active) {
        current = original;
        (void)database.SetActiveSkills(session, current);
      }
    }
  } guard{is_hey_mode, active_skills, original_active_skills, session_id, db_};
  if (input.empty()) return true;
  if (input == "/exit" || input == "/quit") return false;
  if (!config.is_batch_mode) {
    std::string echo = input;
    if (echo.length() > 60) {
      echo = echo.substr(0, 57) + "...";
    }
    std::cout << " " << slop::Colorize(" > " + echo + " ", ansi::EchoBg, ansi::EchoFg) << "\n" << std::endl;
  }
  auto res = cmd_handler_.Handle(
      input, session_id, active_skills, []() { ShowInAppHelp(); }, orchestrator_.GetLastSelectedGroups());
  if (res == CommandHandler::Result::HANDLED || res == CommandHandler::Result::UNKNOWN) {
    return true;
  }
  // 0. Check for hotword prefix "hey <skill> <query>"
  if (absl::StartsWith(input, "hey ")) {
    std::vector<std::string> parts = absl::StrSplit(input, absl::MaxSplits(' ', 2));
    if (parts.size() == 3) {
      hey_skill_name = std::string(parts[1]);
      std::string query = std::string(parts[2]);
      auto exists_or = db_.SkillExists(hey_skill_name);
      if (exists_or.ok() && *exists_or) {
        is_hey_mode = true;
        // Check if skill is already active
        bool already_active = false;
        for (const auto& s : active_skills) {
          if (absl::EqualsIgnoreCase(s, hey_skill_name)) {
            already_active = true;
            break;
          }
        }
        if (!already_active) {
          active_skills.push_back(hey_skill_name);
          (void)db_.SetActiveSkills(session_id, active_skills);
        }
        (void)db_.IncrementSkillActivationCount(hey_skill_name);
        input = std::move(query);
      } else {
        slop::PrintMarkdown(absl::StrCat(
            "### Skill Hotword: 'hey'\n", "The 'hey' hotword allows you to activate a skill for a single prompt.\n\n",
            "**Usage:** `hey <skill_name> <query>`\n", "**Example:** `hey code_reviewer review this patchset.`\n\n",
            (hey_skill_name.empty() ? "" : absl::StrCat("Error: Skill '**", hey_skill_name, "**' not found.\n\n")),
            "To see available skills, use `/skill list`."));
        return true;
      }
    } else {
      slop::PrintMarkdown(absl::StrCat(
          "### Skill Hotword: 'hey'\n", "The 'hey' hotword allows you to activate a skill for a single prompt.\n\n",
          "**Usage:** `hey <skill_name> <query>`\n", "**Example:** `hey code_reviewer review this patchset.`\n\n",
          "To see available skills, use `/skill list`."));
      return true;
    }
  }
  tool_executor_.SetSessionId(session_id);
  tool_executor_.SetMailMode(cmd_handler_.IsMailMode());
  // Execute interaction
  std::string group_id = std::to_string(absl::ToUnixNanos(absl::Now()));
  (void)db_.AppendMessage(session_id, "user", input, "", "completed", group_id, "openai");

  struct AskState {
    absl::Mutex mutex;
    bool asking_user ABSL_GUARDED_BY(mutex) = false;
    std::string prompt ABSL_GUARDED_BY(mutex);
    std::string response ABSL_GUARDED_BY(mutex);
  };

  auto install_ask_user_handler = [&](AskState& ask_state) {
    tool_executor_.SetAskUserHandler([&ask_state](const std::string& prompt) -> std::string {
      absl::MutexLock lock(ask_state.mutex);
      ask_state.prompt = prompt;
      ask_state.asking_user = true;
      ask_state.mutex.Await(absl::Condition(
          +[](AskState* state) ABSL_EXCLUSIVE_LOCKS_REQUIRED(state->mutex) { return !state->asking_user; },
          &ask_state));
      return ask_state.response;
    });
  };

  auto maybe_handle_ask_user_prompt = [&](AskState& ask_state, const std::function<void()>& before_read,
                                          const std::function<void()>& after_read) -> bool {
    std::string current_prompt;
    {
      absl::MutexLock lock(ask_state.mutex);
      if (!ask_state.asking_user) {
        return false;
      }
      current_prompt = ask_state.prompt;
    }

    if (before_read) {
      before_read();
    }
    std::cout << "\n" << ansi::Yellow << "Agent asks:\n" << ansi::Reset;
    slop::Renderer::Get().PrintMarkdown(current_prompt);
    if (!absl::EndsWith(current_prompt, "\n")) {
      std::cout << "\n";
    }
    std::string response = slop::ReadLine("reply");
    if (after_read) {
      after_read();
    }

    {
      absl::MutexLock lock(ask_state.mutex);
      ask_state.response = response;
      ask_state.asking_user = false;
    }
    return true;
  };

  bool context_overflow_retried = false;
  while (true) {
    auto prompt_or = orchestrator_.AssemblePrompt(session_id, active_skills);
    if (!prompt_or.ok()) {
      slop::HandleStatus(prompt_or.status(), "Prompt Error");
      break;
    }
    std::vector<std::string> headers = {"Content-Type: application/json"};
    headers.push_back(std::string("User-Agent: ") + kUserAgent);
    std::string url;
    {
      std::string bearer_token = config.openai_api_key;
      if (config.openai_oauth && oauth_handler_) {
        auto token_or = oauth_handler_->GetValidToken();
        if (!token_or.ok()) {
          slop::HandleStatus(token_or.status(), "OpenAI OAuth Error");
          break;
        }
        bearer_token = *token_or;
        auto account_id_or = oauth_handler_->GetOpenAiAccountId();
        if (account_id_or.ok() && !account_id_or->empty()) {
          headers.push_back("ChatGPT-Account-Id: " + *account_id_or);
        }
      }
      headers.push_back("Authorization: Bearer " + bearer_token);
      const std::string default_openai_base_url =
          config.openai_oauth ? std::string(slop::kOpenAiChatGptCodexBaseUrl) : std::string(slop::kOpenAIBaseUrl);
      const std::string resolved_openai_base_url =
          !config.openai_base_url.empty() ? config.openai_base_url : default_openai_base_url;
      url = resolved_openai_base_url + "/responses";
    }
    auto http_cancellation = std::make_shared<slop::CancellationRequest>();
    http_cancellation->RegisterCallback([&]() { http_client_.Abort(); });

    std::atomic<bool> http_done{false};
    std::atomic<bool> received_stream_text{false};
    absl::Mutex stream_text_mu;
    std::string pending_stream_text;
    absl::StatusOr<std::string> resp_or;
    ResponsesEventDecoder responses_decoder;

    std::string post_url = url;
    std::string post_body = prompt_or->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::vector<std::string> post_headers = headers;
    const bool is_streaming_response = json_get_or(*prompt_or, "stream", false);

    AskState ask_state;
    install_ask_user_handler(ask_state);

    auto collect_stream_text = [&](const std::vector<ResponsesEvent>& events) {
      for (const ResponsesEvent& event : events) {
        if (event.type == ResponsesEventType::kTextDelta && !event.text.empty()) {
          absl::MutexLock lock(stream_text_mu);
          pending_stream_text.append(event.text);
          received_stream_text = true;
        }
      }
    };

    std::thread http_t([&]() {
      resp_or = http_client_.PostStream(post_url, post_body, post_headers, [&](absl::string_view chunk) {
        if (!is_streaming_response) return absl::OkStatus();
        auto events_or = responses_decoder.Feed(chunk);
        if (!events_or.ok()) return events_or.status();
        collect_stream_text(*events_or);
        return absl::OkStatus();
      });
      if (resp_or.ok() && is_streaming_response) {
        auto final_events_or = responses_decoder.Finish();
        if (!final_events_or.ok()) {
          resp_or = final_events_or.status();
        } else {
          collect_stream_text(*final_events_or);
          auto normalized = ResponsesEventDecoder::NormalizeSsePayload(*resp_or);
          if (!normalized.has_value()) {
            resp_or = absl::InvalidArgumentError("Failed to normalize Responses SSE payload");
          } else {
            resp_or = json_dump(*normalized);
          }
        }
      }
      http_done = true;
    });

    {
      std::unique_ptr<slop::ScopedRawMode> raw;
      if (!config.silent) {
        raw = std::make_unique<slop::ScopedRawMode>();
      }

      slop::AsyncAnimator animator;
      if (!config.silent) animator.Start();

      while (!http_done) {
        std::string stream_text;
        {
          absl::MutexLock lock(stream_text_mu);
          stream_text.swap(pending_stream_text);
        }
        if (!stream_text.empty() && !config.silent) {
          animator.Stop();
          slop::PrintAssistantTextDelta(stream_text, "  ");
        }
        if (maybe_handle_ask_user_prompt(
                ask_state,
                [&]() {
                  if (!config.silent) {
                    animator.Stop();
                    raw.reset();
                  }
                },
                [&]() {
                  if (!config.silent) {
                    raw = std::make_unique<slop::ScopedRawMode>();
                    animator.Start();
                  }
                })) {
          continue;
        }
        if (!config.silent && slop::IsInterruptPressed()) {
          animator.Stop();
          http_cancellation->Cancel();
          std::cout << "\n" << slop::Colorize("[Esc/Ctrl-C] Cancelling HTTP request...", "", ansi::Red) << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      std::string final_stream_text;
      {
        absl::MutexLock lock(stream_text_mu);
        final_stream_text.swap(pending_stream_text);
      }
      if (!final_stream_text.empty() && !config.silent) {
        animator.Stop();
        slop::PrintAssistantTextDelta(final_stream_text, "  ");
      }

      // Cleanup handler
      tool_executor_.SetAskUserHandler(nullptr);

      if (!config.silent) animator.Stop();
    }
    http_t.join();
    if (!resp_or.ok()) {
      if (resp_or.status().code() == absl::StatusCode::kResourceExhausted && !context_overflow_retried) {
        context_overflow_retried = true;
        const absl::Status reset_status = orchestrator_.ForceAccordionReset(session_id);
        if (!reset_status.ok()) {
          slop::HandleStatus(reset_status, "Accordion reset error");
          break;
        }
        LOG(WARNING) << "Provider context overflow; reset accordion history and retrying once.";
        continue;
      }
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
            (void)db_.AppendMessage(session_id, "user", "History auto-fixed by dropping problematic tool calls.");
            continue;
          }
        }
      }
      if (!resp_or.ok()) {
        if (resp_or.status().code() == absl::StatusCode::kResourceExhausted) {
          std::cerr << "Context remains too large after accordion reset. Use /context <retain_groups> "
                    << "<watermark_tokens> with a watermark near 80% of model context capacity, or reduce retain groups."
                    << std::endl;
        }
        slop::HandleStatus(resp_or.status(), "HTTP Error");
        if (config.openai_oauth && oauth_handler_ &&
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
    absl::flat_hash_set<std::string> enabled_tool_names;
    auto enabled_tools_or = db_.GetTopLevelTools();
    if (enabled_tools_or.ok()) {
      for (const auto& t : *enabled_tools_or) {
        enabled_tool_names.insert(t.name);
      }
    }
    for (size_t i = start_idx; i < history_after_or->size(); ++i) {
      const auto& msg = (*history_after_or)[i];
      if (!config.silent &&
          !(received_stream_text && msg.role == "assistant" && msg.status != "tool_call")) {
        slop::PrintMessage(msg);
      }
      if (msg.role == "assistant") {
        auto calls_or = orchestrator_.ParseToolCalls(msg);
        if (calls_or.ok() && !calls_or->empty()) {
          std::vector<slop::ToolDispatcher::Call> dispatcher_calls;
          std::vector<slop::ToolDispatcher::Result> results;
          for (const auto& call : *calls_or) {
            std::string combined_id = call.id;
            if (call.id != call.name && !absl::StrContains(call.id, '|')) {
              combined_id = call.id + "|" + call.name;
            }
            if (enabled_tool_names.find(call.name) == enabled_tool_names.end()) {
              results.push_back({combined_id, call.name, absl::NotFoundError("Tool not found: " + call.name + ".")});
              continue;
            }
            dispatcher_calls.push_back({combined_id, call.name, call.args});
          }
          auto cancellation = std::make_shared<slop::CancellationRequest>();
          AskState ask_state;
          install_ask_user_handler(ask_state);

          std::atomic<bool> done{false};
          std::thread t([&] {
            if (!dispatcher_calls.empty()) {
              auto dispatched = dispatcher_.Dispatch(dispatcher_calls, cancellation, orchestrator_.GetThrottle());
              results.insert(results.end(), dispatched.begin(), dispatched.end());
            }
            done = true;
          });
          {
            std::unique_ptr<slop::ScopedRawMode> raw;
            if (!config.silent) {
              raw = std::make_unique<slop::ScopedRawMode>();
            }
            while (!done) {
              if (maybe_handle_ask_user_prompt(
                      ask_state, [&]() { raw.reset(); },
                      [&]() {
                        if (!config.silent) {
                          raw = std::make_unique<slop::ScopedRawMode>();
                        }
                      })) {
              } else {
                if (!config.silent && slop::IsInterruptPressed()) {
                  cancellation->Cancel();
                  std::cerr << "\n"
                            << "  " << slop::Colorize("[Esc/Ctrl-C] Cancellation requested...", "", ansi::Red)
                            << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
              }
            }
          }
          t.join();
          tool_executor_.SetAskUserHandler(nullptr);
          for (const auto& res : results) {
            std::string result_content =
                res.output.ok() ? *res.output : absl::StrCat("Error: ", res.output.status().message());
            if (!config.silent) {
              slop::PrintToolResultMessage(res.name, result_content, res.output.ok() ? "completed" : "error", "  ");
            }
            (void)db_.AppendMessage(session_id, "tool", result_content, res.id, res.output.ok() ? "completed" : "error",
                                    group_id, msg.parsing_strategy);
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

InteractionEngine::PromptRunResult InteractionEngine::ProcessPrompt(std::string input, std::string session_id,
                                                                    std::vector<std::string> active_skills,
                                                                    Config config) {
  const absl::Time start = absl::Now();
  config.is_batch_mode = true;
  PromptRunResult result;
  result.session_id = session_id;
  result.model = orchestrator_.GetModel();
  result.active_skills = active_skills;

  const bool process_ok = Process(input, session_id, active_skills, config);
  result.session_id = session_id;
  result.active_skills = active_skills;
  result.duration_ms = absl::ToInt64Milliseconds(absl::Now() - start);
  if (!process_ok) {
    result.ok = false;
    result.error_code = "cancelled";
    result.error_message = "Prompt processing did not complete.";
    return result;
  }

  auto history_or = db_.GetConversationHistory(session_id, false, 1);
  if (!history_or.ok()) {
    result.ok = false;
    result.error_code = "internal";
    result.error_message = std::string(history_or.status().message());
    return result;
  }
  if (history_or->empty() || history_or->back().role != "assistant") {
    result.ok = false;
    result.error_code = "not_found";
    result.error_message = "No assistant response found.";
    return result;
  }

  result.ok = true;
  result.assistant_message = history_or->back().content;
  return result;
}

absl::StatusOr<InteractionEngine::QueryOptions> InteractionEngine::NormalizeQueryOptions(const QueryOptions& options) {
  QueryOptions normalized = options;
  if (normalized.session_id.empty()) {
    normalized.session_id = "query";
  }
  if (normalized.skill.has_value() && normalized.skill->empty()) {
    return absl::InvalidArgumentError("QueryOptions.skill must be non-empty when provided");
  }
  if (normalized.context_window.has_value() && *normalized.context_window < 0) {
    return absl::InvalidArgumentError("QueryOptions.context_window must be >= 0 when provided");
  }
  if (!IsValidQueryExecutionContext(normalized)) {
    return absl::InvalidArgumentError("QueryOptions execution context is invalid");
  }
  return normalized;
}

bool InteractionEngine::IsValidQueryExecutionContext(const QueryOptions& options) {
  if (options.execution_depth < 0 || options.execution_depth > 1) {
    return false;
  }
  if (options.execution_scope == QueryOptions::ExecutionScope::kRoot) {
    return options.execution_depth == 0;
  }
  return options.execution_depth == 1;
}

absl::StatusOr<std::string> InteractionEngine::Query(const std::string& prompt, const Config& config,
                                                     const std::vector<std::string>& active_skills) {
  QueryOptions options;
  return Query(prompt, config, active_skills, options);
}

absl::StatusOr<std::string> InteractionEngine::Query(const std::string& prompt, const Config& config,
                                                      const std::vector<std::string>& active_skills,
                                                      const QueryOptions& options) {
  ASSIGN_OR_RETURN(QueryOptions normalized_options, NormalizeQueryOptions(options));
  const QueryOptions& effective_options = normalized_options;

  Database transient_db;
  auto status = transient_db.Init(":memory:");
  if (!status.ok()) return status;

  const std::string query_session_id = effective_options.session_id;
  // Transient queries use the default accordion context settings.
  (void)transient_db.Execute("UPDATE tools SET is_enabled = 0 WHERE name = 'ask_user'");

  auto sub_orch_or = orchestrator_.Update().WithDatabase(&transient_db).Build();
  if (!sub_orch_or.ok()) return sub_orch_or.status();

  auto sub_tool_executor_or = ToolExecutor::Create(&transient_db);
  if (!sub_tool_executor_or.ok()) return sub_tool_executor_or.status();

  if (effective_options.skill.has_value() && !effective_options.skill->empty()) {
    auto skills_or = db_.GetSkills();
    if (skills_or.ok()) {
      for (const auto& s : *skills_or) {
        if (s.name == *effective_options.skill) {
          (void)transient_db.RegisterSkill({0, s.name, s.description, s.system_prompt_patch, 0});
          break;
        }
      }
    }
  }
  auto sub_tool_executor = std::move(*sub_tool_executor_or);
  sub_tool_executor->SetSessionId(query_session_id);
  sub_tool_executor->SetMailMode(cmd_handler_.IsMailMode());
  sub_tool_executor->SetExecutionContext(effective_options.execution_scope == QueryOptions::ExecutionScope::kSubquery
                                             ? ToolExecutor::ExecutionScope::kSubquery
                                             : ToolExecutor::ExecutionScope::kRoot,
                                         effective_options.execution_depth);

  sub_tool_executor->SetDispatcher(std::make_unique<ToolDispatcher>(
      [executor = sub_tool_executor.get()](const std::string& name, const nlohmann::json& args,
                                           std::shared_ptr<CancellationRequest> cancellation) {
        return executor->Execute(name, args, cancellation);
      }));

  InteractionEngine sub_engine(transient_db, **sub_orch_or, cmd_handler_, *sub_tool_executor->dispatcher(),
                               *sub_tool_executor, http_client_, oauth_handler_);
  Config sub_config = config;
  sub_config.silent = true;
  std::string input = prompt;
  std::string session_id = query_session_id;
  std::vector<std::string> skills = active_skills;
  if (effective_options.skill.has_value() && !effective_options.skill->empty() &&
      std::find(skills.begin(), skills.end(), *effective_options.skill) == skills.end()) {
    skills.push_back(*effective_options.skill);
  }
  (void)transient_db.SetActiveSkills(session_id, skills);
  (void)sub_engine.Process(input, session_id, skills, sub_config);
  // Get the last assistant message
  auto history_or = transient_db.GetConversationHistory(session_id, false, 1);
  if (history_or.ok() && !history_or->empty() && history_or->back().role == "assistant") {
    return history_or->back().content;
  }
  return absl::NotFoundError("No assistant response found");
}
}  // namespace slop
