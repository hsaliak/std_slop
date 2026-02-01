#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"

#include "core/constants.h"
#include "core/database.h"
#include "core/http_client.h"
#include "core/oauth_handler.h"
#include "core/cancellation.h"
#include "core/orchestrator.h"
#include "core/shell_util.h"
#include "core/tool_dispatcher.h"
#include "core/tool_executor.h"
#include "interface/color.h"
#include "interface/command_definitions.h"
#include "interface/command_handler.h"
#include "interface/ui.h"

ABSL_FLAG(std::string, db, "slop.db", "Path to SQLite database");
ABSL_FLAG(std::string, log, "", "Log file path");
ABSL_FLAG(bool, google_oauth, false, "Use Google OAuth for authentication");
ABSL_FLAG(std::string, project, "", "Set Google Cloud Project ID for OAuth mode");
ABSL_FLAG(std::string, model, "", "Model name (overrides GEMINI_MODEL or OPENAI_MODEL env vars)");
ABSL_FLAG(std::string, google_api_key, "", "Google API key (overrides GOOGLE_API_KEY env var)");
ABSL_FLAG(std::string, openai_api_key, "", "OpenAI API key (overrides OPENAI_API_KEY env var)");
ABSL_FLAG(std::string, openai_base_url, "", "OpenAI Base URL (overrides OPENAI_BASE_URL env var)");
ABSL_FLAG(bool, strip_reasoning, false,
          "Strip reasoning from OpenAI-compatible API responses (Recommended when using newer models via OpenRouter to "
          "improve response speed and focus)");

ABSL_FLAG(int, max_parallel_tools, 4, "Maximum number of tools to execute in parallel");

std::string GetHelpText() {
  std::string help =
      "# std::slop - The SQL-backed LLM CLI\n\n"
      "## Usage\n"
      "```bash\n"
      "std_slop [session_id] [options]\n"
      "```\n\n"
      "## Options\n"
      "Use `--helpfull` to see all available command-line flags.\n\n"
      "## Slash Commands\n\n";

  std::map<std::string, std::vector<std::pair<std::string, std::string>>> category_rows;
  std::vector<std::string> categories;

  for (const auto& def : slop::GetCommandDefinitions()) {
    if (std::find(categories.begin(), categories.end(), def.category) == categories.end()) {
      categories.push_back(def.category);
    }
    for (const auto& line : def.help_lines) {
      if (line.empty()) continue;
      if (line[0] == '/') {
        size_t sep = line.find("  ");
        if (sep != std::string::npos) {
          category_rows[def.category].emplace_back(line.substr(0, sep),
                                                   std::string(absl::StripLeadingAsciiWhitespace(line.substr(sep))));
        } else {
          category_rows[def.category].emplace_back(line, "");
        }
      } else {
        std::string name_part = def.name;
        for (const auto& alias : def.aliases) {
          name_part += ", " + alias;
        }
        category_rows[def.category].emplace_back(name_part, line);
      }
    }
  }

  for (const auto& cat : categories) {
    help += "### " + cat + "\n\n";
    help += "| Command | Description |\n";
    help += "| :--- | :--- |\n";

    for (const auto& row : category_rows[cat]) {
      // Escape pipes in markdown
      std::string cmd = absl::StrReplaceAll(row.first, {{"|", "\\|"}});
      std::string desc = absl::StrReplaceAll(row.second, {{"|", "\\|"}});
      help += absl::Substitute("| `$0` | $1 |\n", cmd, desc);
    }
    help += "\n";
  }

  return help;
}

void ShowHelp() { slop::PrintMarkdown(GetHelpText()); }

namespace {

class FileLogSink : public absl::LogSink {
 public:
  explicit FileLogSink(const std::string& path) : stream_(path, std::ios::app) {
    if (!stream_.is_open()) {
      std::cerr << "Failed to open log file: " << path << std::endl;
    }
  }
  ~FileLogSink() override = default;

  void Send(const absl::LogEntry& entry) override {
    if (stream_.is_open()) {
      std::lock_guard<std::mutex> lock(mu_);
      stream_ << entry.text_message_with_prefix() << "\n";
    }
  }

 private:
  // Mutex is required because Abseil's LogSink::Send can be called from multiple
  // threads even if the main application logic is currently single-threaded
  // (e.g., internal library threads or future asynchronous components).
  std::mutex mu_;
  std::ofstream stream_;
};

}  // namespace

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(GetHelpText());
  std::vector<char*> positional_args = absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  std::string log_path = absl::GetFlag(FLAGS_log);
  std::unique_ptr<FileLogSink> log_sink;
  if (!log_path.empty()) {
    log_sink = std::make_unique<FileLogSink>(log_path);
    absl::AddLogSink(log_sink.get());
  }
  LOG(INFO) << "Logging initialized and sink added.";

  std::string session_id = "default_session";
  if (positional_args.size() > 1) {
    session_id = positional_args[1];
  }

  std::string db_path = absl::GetFlag(FLAGS_db);
  bool google_auth = absl::GetFlag(FLAGS_google_oauth);
  std::string manual_project_id = absl::GetFlag(FLAGS_project);
  std::string flag_model = absl::GetFlag(FLAGS_model);
  std::string flag_google_api_key = absl::GetFlag(FLAGS_google_api_key);
  std::string flag_openai_api_key = absl::GetFlag(FLAGS_openai_api_key);
  std::string flag_openai_base_url = absl::GetFlag(FLAGS_openai_base_url);

  const char* env_google_key = std::getenv("GOOGLE_API_KEY");
  const char* env_openai_key = std::getenv("OPENAI_API_KEY");
  const char* env_openai_base_url = std::getenv("OPENAI_BASE_URL");

  std::string google_key = !flag_google_api_key.empty() ? flag_google_api_key : (env_google_key ? env_google_key : "");
  std::string openai_key = !flag_openai_api_key.empty() ? flag_openai_api_key : (env_openai_key ? env_openai_key : "");
  std::string openai_base_url =
      !flag_openai_base_url.empty() ? flag_openai_base_url : (env_openai_base_url ? env_openai_base_url : "");

  std::string model = flag_model;
  if (model.empty()) {
    const char* env_gemini = std::getenv("GEMINI_MODEL");
    const char* env_openai = std::getenv("OPENAI_MODEL");
    if (env_gemini)
      model = env_gemini;
    else if (env_openai)
      model = env_openai;
  }

  if (!google_auth && google_key.empty() && openai_key.empty()) {
    google_auth = true;
    std::cout << "No API keys found. Defaulting to Google OAuth mode." << std::endl;
  }

  slop::Database db;
  auto status = db.Init(db_path);
  if (!status.ok()) {
    slop::HandleStatus(status, "Database Error");
    return 1;
  }

  slop::HttpClient http_client;
  slop::Orchestrator::Builder builder(&db, &http_client);
  builder.WithStripReasoning(absl::GetFlag(FLAGS_strip_reasoning));

  if (google_auth) {  // google OAuth
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
        .WithModel(!model.empty() ? model : "gemini-3-flash-preview")
        .WithBaseUrl(absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal"))
        .WithGcaMode(true);
  } else if (!openai_key.empty()) {  // openAI API key
    builder.WithProvider(slop::Orchestrator::Provider::OPENAI)
        .WithModel(!model.empty() ? model : "gpt-4o")
        .WithBaseUrl(!openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl);
  } else {  // gemini API key
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
        .WithModel(!model.empty() ? model : "gemini-3-flash-preview")
        .WithBaseUrl(slop::kPublicGeminiBaseUrl);
  }

  auto orchestrator_or = builder.Build();
  if (!orchestrator_or.ok()) {
    LOG(ERROR) << "Failed to create orchestrator: " << orchestrator_or.status().message();
    return 1;
  }
  auto orchestrator = std::move(*orchestrator_or);

  std::unique_ptr<slop::OAuthHandler> oauth_handler;
  if (google_auth) {
    oauth_handler = std::make_unique<slop::OAuthHandler>(&http_client);
    if (!manual_project_id.empty()) {
      oauth_handler->SetProjectId(manual_project_id);
    }
    oauth_handler->SetEnabled(true);
    auto token_or = oauth_handler->GetValidToken();
    if (!token_or.ok()) {
      if (absl::IsUnauthenticated(token_or.status()) || absl::IsNotFound(token_or.status())) {
        std::cout << "Google OAuth: " << token_or.status().message() << std::endl;
        std::cout << "Please run ./slop_auth.sh to authenticate." << std::endl;
        return 1;
      }
    }
    auto proj_or = oauth_handler->GetProjectId();
    if (proj_or.ok()) {
      orchestrator->Update().WithProjectId(*proj_or).BuildInto(orchestrator.get());
    }
  }

  auto tool_executor_or = slop::ToolExecutor::Create(&db);
  if (!tool_executor_or.ok()) {
    LOG(ERROR) << "Failed to create tool executor: " << tool_executor_or.status().message();
    return 1;
  }
  auto& tool_executor = **tool_executor_or;
  tool_executor.SetSessionId(session_id);

  slop::ToolDispatcher dispatcher(
      [&](const std::string& name, const nlohmann::json& args,
          std::shared_ptr<slop::CancellationRequest> cancellation) {
        return tool_executor.Execute(name, args, cancellation);
      },
      absl::GetFlag(FLAGS_max_parallel_tools));
  auto cmd_handler_or =
      slop::CommandHandler::Create(&db, orchestrator.get(), oauth_handler.get(), google_key, openai_key);
  if (!cmd_handler_or.ok()) {
    LOG(ERROR) << "Failed to create command handler: " << cmd_handler_or.status().message();
    return 1;
  }
  auto& cmd_handler = **cmd_handler_or;
  slop::SetCompletionCommands(cmd_handler.GetCommandNames(), cmd_handler.GetSubCommandMap());
  std::vector<std::string> active_skills;
  auto active_skills_or = db.GetActiveSkills(session_id);
  if (active_skills_or.ok()) {
    active_skills = *active_skills_or;
  }

  slop::SetupTerminal();
  slop::ShowBanner();
  std::cout << slop::Colorize("std::slop", "", ansi::Logo) << " - Session: " << session_id << " ("
            << orchestrator->GetModel() << ")" << std::endl;
  std::cout << "Type /help for slash commands." << std::endl;

  (void)slop::DisplayHistory(db, session_id, 20);
  (void)orchestrator->RebuildContext(session_id);

  while (true) {
    auto current_skills_or = db.GetActiveSkills(session_id);
    if (current_skills_or.ok()) {
      active_skills = *current_skills_or;
    }

    auto settings_or = db.GetContextSettings(session_id);
    int window_size = settings_or.ok() ? settings_or->size : 0;
    std::string model_name = orchestrator->GetModel();
    std::string persona = active_skills.empty() ? "default" : absl::StrJoin(active_skills, ",");
    std::string window_str = (window_size == 0) ? "all" : std::to_string(window_size);
    std::string modeline = absl::StrCat("std::slop<W:", window_str, ", M:", model_name, ", P:", persona,
                                        ", S:", session_id, ", T:", orchestrator->GetThrottle(), "s>");

    std::string input = slop::ReadLine(modeline);
    if (input == "/exit" || input == "/quit") break;
    if (input.empty()) continue;

    // Print user echo
    std::string echo = input;
    if (echo.length() > 60) {
      echo = echo.substr(0, 57) + "...";
    }
    std::cout << " " << slop::Colorize(" > " + echo + " ", ansi::EchoBg, ansi::EchoFg) << "\n" << std::endl;

    auto res = cmd_handler.Handle(input, session_id, active_skills, ShowHelp, orchestrator->GetLastSelectedGroups());
    tool_executor.SetSessionId(session_id);
    if (res == slop::CommandHandler::Result::HANDLED || res == slop::CommandHandler::Result::UNKNOWN) {
      continue;
    }

    // Execute interaction
    std::string group_id = std::to_string(absl::ToUnixNanos(absl::Now()));
    (void)db.AppendMessage(session_id, "user", input, "", "completed", group_id, orchestrator->GetName());

    while (true) {
      auto prompt_or = orchestrator->AssemblePrompt(session_id, active_skills);
      if (!prompt_or.ok()) {
        slop::HandleStatus(prompt_or.status(), "Prompt Error");
        break;
      }

      std::vector<std::string> headers = {"Content-Type: application/json"};
      std::string url;

      if (orchestrator->GetProvider() == slop::Orchestrator::Provider::OPENAI) {
        headers.push_back("Authorization: Bearer " + openai_key);
        url = (!openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl) + "/chat/completions";
      } else if (google_auth) {
        auto token_or = oauth_handler->GetValidToken();
        if (token_or.ok()) headers.push_back("Authorization: Bearer " + *token_or);
        url = absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal:generateContent");
      } else {
        headers.push_back("x-goog-api-key: " + google_key);
        url = absl::StrCat(slop::kPublicGeminiBaseUrl, "/models/", orchestrator->GetModel(),
                           ":generateContent?key=", google_key);
      }

      auto resp_or =
          http_client.Post(url, prompt_or->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), headers);
      if (!resp_or.ok()) {
        if (resp_or.status().code() == absl::StatusCode::kInvalidArgument) {
          LOG(WARNING) << "HTTP 400 error detected. Attempting to auto-fix history...";
          auto history_or = db.GetConversationHistory(session_id, false, 10);
          if (history_or.ok() && !history_or->empty()) {
            // Find the most recent tool call and drop it.
            bool dropped = false;
            for (auto it = history_or->rbegin(); it != history_or->rend(); ++it) {
              if (it->status == "tool_call" || it->role == "tool") {
                LOG(INFO) << "Dropping message " << it->id << " to fix 400 error.";
                (void)db.UpdateMessageStatus(it->id, "dropped");
                dropped = true;

                // Also drop its pair if it's a tool response/call
                if (it->role == "tool") {
                  // Try to find the matching tool_call
                  for (auto it2 = it + 1; it2 != history_or->rend(); ++it2) {
                    if (it2->status == "tool_call" && it2->tool_call_id == it->tool_call_id) {
                      LOG(INFO) << "Dropping matching tool call " << it2->id;
                      (void)db.UpdateMessageStatus(it2->id, "dropped");
                      break;
                    }
                  }
                }
                break;  // Only drop the most recent pair
              }
            }

            if (!dropped) {
              // If no tool call was found, maybe it's just too much context.
              // Try to drop the oldest message in the window?
              // Or just let it fail.
            }

            // Re-assemble and retry
            prompt_or = orchestrator->AssemblePrompt(session_id, active_skills);
            if (prompt_or.ok()) {
              std::cout << slop::Colorize("Retrying with adjusted history...", "", ansi::Warning) << std::endl;
              resp_or = http_client.Post(url, prompt_or->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                         headers);
            }
          }
        }

        if (!resp_or.ok()) {
          slop::HandleStatus(resp_or.status(), "HTTP Error");
          if (google_auth &&
              (absl::IsUnauthenticated(resp_or.status()) || absl::IsPermissionDenied(resp_or.status()))) {
            std::cout << "Refreshing OAuth token..." << std::endl;
            (void)oauth_handler->GetValidToken();
          }
          break;
        }
      }

      auto history_before_or = db.GetMessagesByGroups({group_id});
      size_t start_idx = history_before_or.ok() ? history_before_or->size() : 0;

      auto process_or = orchestrator->ProcessResponse(session_id, *resp_or, group_id);
      if (!process_or.ok()) {
        slop::HandleStatus(process_or.status(), "Process Error");
        break;
      }
      (void)*process_or;

      auto history_after_or = db.GetMessagesByGroups({group_id});
      if (!history_after_or.ok() || history_after_or->empty()) break;

      bool has_tool_calls = false;
      for (size_t i = start_idx; i < history_after_or->size(); ++i) {
        const auto& msg = (*history_after_or)[i];
        slop::PrintMessage(msg);

        if (msg.role == "assistant" && msg.status == "tool_call") {
          auto calls_or = orchestrator->ParseToolCalls(msg);
          if (calls_or.ok()) {
            if (!calls_or->empty()) {
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
                results = dispatcher.Dispatch(dispatcher_calls, cancellation);
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
                std::string result =
                    res.output.ok() ? *res.output : absl::StrCat("Error: ", res.output.status().message());
                slop::PrintToolResultMessage(res.name, result, res.output.ok() ? "completed" : "error", "  ");
                (void)db.AppendMessage(session_id, "tool", result, res.id,
                                       res.output.ok() ? "completed" : "error", group_id,
                                       msg.parsing_strategy);
              }
            }
            has_tool_calls = true;
          }
        }
      }

      if (has_tool_calls) {
        if (orchestrator->GetThrottle() > 0) {
          std::this_thread::sleep_for(std::chrono::seconds(orchestrator->GetThrottle()));
        }
        continue;  // Loop for next LLM turn
      }
      break;
    }
  }

  if (log_sink) {
    absl::RemoveLogSink(log_sink.get());
  }
  return 0;
}
