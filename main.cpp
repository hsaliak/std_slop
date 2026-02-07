#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/flags.h"
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
#include "absl/time/time.h"

#include "core/cancellation.h"
#include "core/constants.h"
#include "core/database.h"
#include "core/http_client.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "core/tool_dispatcher.h"
#include "core/tool_executor.h"
#include "interface/color.h"
#include "interface/command_handler.h"
#include "interface/interaction_engine.h"
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
ABSL_FLAG(std::string, session, "", "Session name (overrides positional session_id)");
ABSL_FLAG(std::string, prompt, "", "Run a single prompt in batch mode and exit");

// Help text is now in interface/ui.h

namespace {

class FileLogSink : public absl::LogSink {
 public:
  explicit FileLogSink(const std::string& path) : file_(path, std::ios::app) {}
  void Send(const absl::LogEntry& entry) override {
    if (file_.is_open()) {
      std::lock_guard<std::mutex> lock(mu_);
      file_ << entry.text_message_with_prefix() << std::endl;
    }
  }

 private:
  std::mutex mu_;
  std::ofstream file_;
};

std::vector<std::string> GetActiveSkills(slop::Database& db, const std::string& session_id) {
  auto active_skills_or = db.GetActiveSkills(session_id);
  if (active_skills_or.ok()) {
    return *active_skills_or;
  }
  return {};
}

void RunInteractiveLoop(slop::InteractionEngine& engine, slop::Database& db, slop::Orchestrator& orchestrator,
                        slop::ToolExecutor& tool_executor, std::string& session_id,
                        const slop::InteractionEngine::Config& engine_config) {
  slop::SetupTerminal();
  slop::ShowBanner();
  std::cout << slop::Colorize("std::slop", "", ansi::Logo) << " - Session: " << session_id << " ("
            << orchestrator.GetModel() << ")" << std::endl;
  std::cout << "Type /help for slash commands." << std::endl;

  (void)slop::DisplayHistory(db, session_id, 20);
  (void)orchestrator.RebuildContext(session_id);

  while (true) {
    std::vector<std::string> active_skills = GetActiveSkills(db, session_id);

    auto settings_or = db.GetContextSettings(session_id);
    int window_size = settings_or.ok() ? settings_or->size : 0;
    std::string model_name = orchestrator.GetModel();
    std::string persona = active_skills.empty() ? "default" : absl::StrJoin(active_skills, ",");
    std::string window_str = (window_size == 0) ? "all" : std::to_string(window_size);
    bool is_mail = engine.GetCommandHandler().IsMailMode();
    std::string color = is_mail ? ansi::Green : ansi::Cyan;
    std::string mode_label = is_mail ? absl::StrCat(icons::Mailbox, " MAIL_MODEL")
                                     : absl::StrCat(icons::Robot, " STANDARD");
    std::string modeline = absl::StrCat(color, "std::slop <", mode_label, " | W:", window_str, ", M:", model_name,
                                        ", P:", persona, ", S:", session_id, ", T:", orchestrator.GetThrottle(), "s>",
                                        ansi::Reset);

    std::string input = slop::ReadLine(modeline);
    tool_executor.SetSessionId(session_id);
    if (!engine.Process(input, session_id, active_skills, engine_config)) {
      break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(slop::GetHelpText());
  (void)absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  std::string log_path = absl::GetFlag(FLAGS_log);
  std::unique_ptr<FileLogSink> log_sink;
  if (!log_path.empty()) {
    log_sink = std::make_unique<FileLogSink>(log_path);
    absl::AddLogSink(log_sink.get());
  }

  std::string db_path = absl::GetFlag(FLAGS_db);
  bool google_auth = absl::GetFlag(FLAGS_google_oauth);
  std::string manual_project_id = absl::GetFlag(FLAGS_project);

  slop::Database db;
  if (auto status = db.Init(db_path); !status.ok()) {
    std::cerr << "Failed to initialize database: " << status.message() << std::endl;
    return 1;
  }

  slop::HttpClient http_client;
  slop::Orchestrator::Builder builder(&db, &http_client);
  builder.WithStripReasoning(absl::GetFlag(FLAGS_strip_reasoning));

  std::string google_key = absl::GetFlag(FLAGS_google_api_key);
  if (google_key.empty()) {
    const char* env_key = std::getenv("GOOGLE_API_KEY");
    if (env_key) google_key = env_key;
  }

  std::string openai_key = absl::GetFlag(FLAGS_openai_api_key);
  if (openai_key.empty()) {
    const char* env_key = std::getenv("OPENAI_API_KEY");
    if (env_key) openai_key = env_key;
  }

  std::string openai_base_url = absl::GetFlag(FLAGS_openai_base_url);
  if (openai_base_url.empty()) {
    const char* env_url = std::getenv("OPENAI_BASE_URL");
    if (env_url) openai_base_url = env_url;
  }

  if (!google_auth && google_key.empty() && openai_key.empty()) {
    google_auth = true;
    std::cout << "No API keys found. Defaulting to Google OAuth mode." << std::endl;
  }

  std::string model = absl::GetFlag(FLAGS_model);
  if (model.empty()) {
    if (!openai_key.empty()) {
      const char* env_model = std::getenv("OPENAI_MODEL");
      if (env_model) model = env_model;
    } else {
      const char* env_model = std::getenv("GEMINI_MODEL");
      if (env_model) model = env_model;
    }
  }

  if (google_auth) {
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
        .WithModel(!model.empty() ? model : "gemini-3-flash-preview")
        .WithBaseUrl(absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal"))
        .WithGcaMode(true);
  } else if (!openai_key.empty()) {
    builder.WithProvider(slop::Orchestrator::Provider::OPENAI)
        .WithModel(!model.empty() ? model : "gpt-4o")
        .WithBaseUrl(!openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl);
  } else {  // gemini API key
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
        .WithModel(!model.empty() ? model : "gemini-3-flash-preview");
  }

  auto orchestrator_or = builder.Build();
  if (!orchestrator_or.ok()) {
    std::cerr << "Failed to initialize orchestrator: " << orchestrator_or.status().message() << std::endl;
    return 1;
  }
  auto orchestrator = std::move(*orchestrator_or);

  std::shared_ptr<slop::OAuthHandler> oauth_handler;
  if (google_auth) {
    oauth_handler = std::make_shared<slop::OAuthHandler>(&http_client);
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
    std::cerr << "Failed to initialize tool executor: " << tool_executor_or.status().message() << std::endl;
    return 1;
  }
  auto tool_executor = std::move(*tool_executor_or);

  slop::ToolDispatcher dispatcher(
      [&tool_executor](const std::string& name, const nlohmann::json& args,
                       std::shared_ptr<slop::CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
        return tool_executor->Execute(name, args, cancellation);
      },
      absl::GetFlag(FLAGS_max_parallel_tools));

  auto cmd_handler_or =
      slop::CommandHandler::Create(&db, orchestrator.get(), oauth_handler.get(), google_key, openai_key);
  if (!cmd_handler_or.ok()) {
    std::cerr << "Failed to initialize command handler: " << cmd_handler_or.status().message() << std::endl;
    return 1;
  }
  auto& cmd_handler = **cmd_handler_or;
  slop::SetCompletionCommands(cmd_handler.GetCommandNames(), cmd_handler.GetSubCommandMap());

  std::string session_id = absl::GetFlag(FLAGS_session);
  if (session_id.empty()) {
    session_id = "default_session";
    std::cout << "Using default session: " << session_id << std::endl;
  }

  std::vector<std::string> active_skills = GetActiveSkills(db, session_id);

  slop::InteractionEngine engine(db, *orchestrator, cmd_handler, dispatcher, *tool_executor, http_client,
                                 oauth_handler);
  slop::InteractionEngine::Config engine_config;
  engine_config.google_api_key = google_key;
  engine_config.openai_api_key = openai_key;
  engine_config.openai_base_url = openai_base_url;
  engine_config.google_oauth = google_auth;

  std::string batch_prompt = absl::GetFlag(FLAGS_prompt);
  if (!batch_prompt.empty()) {
    engine_config.is_batch_mode = true;
    engine.Process(batch_prompt, session_id, active_skills, engine_config);
  } else {
    RunInteractiveLoop(engine, db, *orchestrator, *tool_executor, session_id, engine_config);
  }

  if (log_sink) {
    absl::RemoveLogSink(static_cast<absl::LogSink*>(log_sink.get()));
  }
  return 0;
}
