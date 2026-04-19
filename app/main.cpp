#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
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
#include "core/config.h"
#include "core/constants.h"
#include "core/database.h"
#include "core/http_client.h"
#include "core/json_utils.h"
#include "core/oauth_handler.h"
#include "core/orchestrator.h"
#include "tools/tool_dispatcher.h"
#include "tools/tool_executor.h"
#include "interface/color.h"
#include "app/llm_tool_specializations.h"
#include "interface/command_handler.h"
#include "interface/completer.h"
#include "interface/interaction_engine.h"
#include "interface/terminal.h"
#include "interface/ui.h"

#include <readline/readline.h>

ABSL_FLAG(std::string, config, "", "Path to the configuration INI file");
ABSL_FLAG(std::string, db, "", "Path to SQLite database (default: slop.db)");
ABSL_FLAG(std::string, log, "", "Log file path");
ABSL_FLAG(std::string, project, "", "Set Google Cloud Project ID for OAuth mode");
ABSL_FLAG(std::string, model, "", "Model name (overrides GEMINI_MODEL or OPENAI_MODEL env vars)");
ABSL_FLAG(std::string, google_api_key, "", "Google API key");
ABSL_FLAG(std::string, openai_api_key, "", "OpenAI API key");
ABSL_FLAG(std::string, openai_base_url, "", "OpenAI Base URL");
ABSL_FLAG(bool, use_responses, false, "Use OpenAI Responses API instead of chat completions for OpenAI API key mode");
ABSL_FLAG(bool, openai_oauth, false, "Use OpenAI OAuth token file (~/.config/slop/chatgpt_plus_token.json)");
ABSL_FLAG(std::string, openai_oauth_token_path, "", "Override OpenAI OAuth token file path");
ABSL_FLAG(bool, fetch_openai_oauth_token, false,
          "Fetch an OpenAI OAuth token via built-in browser+paste flow and exit");
ABSL_FLAG(bool, fetch_openai_oauth_device_token, false,
          "Fetch an OpenAI OAuth token via built-in device flow and exit");

ABSL_FLAG(std::string, session, "", "Session name (overrides positional session_id)");
ABSL_FLAG(std::string, prompt, "", "Run a single prompt in batch mode and exit");
ABSL_FLAG(std::string, prompt_db, "",
          "Database to use for batch mode. Defaults to in-memory (':memory:'). Mutually exclusive with --db.");

// Help text is now in interface/ui.h

void SignalHandler(int signum) {
  if (signum == SIGINT) {
    std::cout << "\nUse /quit to quit." << std::endl;
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
  }
}

namespace {

constexpr const char* kWalkthroughUrl = "https://github.com/hsaliak/std_slop/blob/main/docs/WALKTHROUGH.md";

std::string MissingAuthenticationMessage() {
  return absl::StrCat("No authentication method found. Configure at least one authentication method. See the getting "
                      "started walkthrough: ",
                      kWalkthroughUrl);
}

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
    tool_executor.SetSessionId(session_id);
    // Active skills can also be modified outside ToolExecutor (e.g. /skill, /mode).
    // Invalidate once per turn so UI + prompt state always reflect the latest DB value,
    // while still keeping intra-turn cache benefits.
    tool_executor.InvalidateActiveSkillsCache();
    engine.GetCommandHandler().RefreshMailModeFromDb();
    std::vector<std::string> active_skills = tool_executor.GetActiveSkills();

    auto settings_or = db.GetContextSettings(session_id);
    int window_size = settings_or.ok() ? settings_or->size : 0;
    std::string model_name = orchestrator.GetModel();
    std::string persona = active_skills.empty() ? "default" : absl::StrJoin(active_skills, ",");
    std::string window_str = (window_size == 0) ? "all" : std::to_string(window_size);
    bool is_mail = engine.GetCommandHandler().IsMailMode();
    std::string color = is_mail ? ansi::MailMode : ansi::StandardMode;
    std::string mode_label =
        is_mail ? absl::StrCat(icons::Mailbox, " MAIL_MODEL") : absl::StrCat(icons::Robot, " STANDARD");
    std::string modeline =
        absl::StrCat(color, "── std::slop <", mode_label, ", W:", window_str, ", M:", model_name, ", P:", persona,
                     ", S:", session_id, ", T:", orchestrator.GetThrottle(), "s>", ansi::Reset);

    std::string input = slop::ReadLine(modeline);
    if (!engine.Process(input, session_id, active_skills, engine_config)) {
      break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, SignalHandler);
  absl::InitializeSymbolizer(argv[0]);
  absl::InstallFailureSignalHandler(absl::FailureSignalHandlerOptions{});

  absl::SetProgramUsageMessage(slop::GetHelpText());
  (void)absl::ParseCommandLine(argc, argv);

  // Check if --db was specified on the command line before applying config.
  // We use the fact that the default is now an empty string.
  bool db_on_cli = !absl::GetFlag(FLAGS_db).empty();

  slop::LoadConfigAndApply(absl::GetFlag(FLAGS_config));
  auto llm_specializations_or = slop::LoadLlmToolSpecializations(absl::GetFlag(FLAGS_config));
  if (!llm_specializations_or.ok()) {
    std::cerr << "Failed to load llm tool specializations: " << llm_specializations_or.status().message()
              << std::endl;
    return 1;
  }
  absl::InitializeLog();

  const std::vector<slop::LlmToolSpecializationConfig> llm_specializations = *llm_specializations_or;
  std::string log_path = absl::GetFlag(FLAGS_log);
  std::unique_ptr<FileLogSink> log_sink;
  if (!log_path.empty()) {
    log_sink = std::make_unique<FileLogSink>(log_path);
    absl::AddLogSink(log_sink.get());
  }

  std::string prompt = absl::GetFlag(FLAGS_prompt);
  const bool fetch_openai_oauth_token = absl::GetFlag(FLAGS_fetch_openai_oauth_token);
  const bool fetch_openai_oauth_device_token = absl::GetFlag(FLAGS_fetch_openai_oauth_device_token);
  std::string db_path;

  if (!prompt.empty()) {
    // Batch mode defaults to in-memory database unless --prompt-db is specified.
    // --db is forbidden on the command line in batch mode to prevent accidental
    // pollution of the main database.
    if (db_on_cli) {
      std::cerr << "Error: --db and --prompt are mutually exclusive. Use --prompt-db "
                   "if you need a persistent database for a single prompt."
                << std::endl;
      return 1;
    }
    std::string prompt_db = absl::GetFlag(FLAGS_prompt_db);
    db_path = !prompt_db.empty() ? prompt_db : ":memory:";
  } else {
    // Interactive mode. Use --db if specified (CLI or INI), else default to slop.db.
    db_path = absl::GetFlag(FLAGS_db);
    if (db_path.empty()) {
      db_path = "slop.db";
    }
  }

  bool openai_oauth = absl::GetFlag(FLAGS_openai_oauth);
  bool use_responses = absl::GetFlag(FLAGS_use_responses);

  slop::Database db;
  if (auto status = db.Init(db_path); !status.ok()) {
    std::cerr << "Failed to initialize database: " << status.message() << std::endl;
    return 1;
  }

  slop::HttpClient http_client;
  auto configure_openai_oauth_handler = [&http_client](std::shared_ptr<slop::OAuthHandler>* handler) {
    *handler = std::make_shared<slop::OAuthHandler>(&http_client, slop::OAuthHandler::Provider::kOpenAi);
    std::string openai_oauth_token_path = absl::GetFlag(FLAGS_openai_oauth_token_path);
    if (!openai_oauth_token_path.empty()) {
      (*handler)->SetTokenPath(openai_oauth_token_path);
    }
    (*handler)->SetEnabled(true);
  };

  if (fetch_openai_oauth_token && fetch_openai_oauth_device_token) {
    std::cerr << "Choose only one of --fetch_openai_oauth_token or --fetch_openai_oauth_device_token."
              << std::endl;
    return 1;
  }

  if (fetch_openai_oauth_token) {
    std::shared_ptr<slop::OAuthHandler> bootstrap_oauth_handler;
    configure_openai_oauth_handler(&bootstrap_oauth_handler);
    auto session_or = bootstrap_oauth_handler->StartOpenAiManualAuthorization();
    if (!session_or.ok()) {
      std::cerr << "Failed to start OpenAI OAuth browser flow: " << session_or.status().message() << std::endl;
      return 1;
    }
    std::cout << "OpenAI OAuth browser flow\n"
              << "1. Open: " << session_or->authorization_uri << "\n"
              << "2. Finish login/consent in the browser.\n"
              << "3. Paste the full redirect URL here.\n\n"
              << "Redirect URL: ";
    std::string callback_url;
    std::getline(std::cin, callback_url);
    auto fetch_status = bootstrap_oauth_handler->CompleteOpenAiManualAuthorization(*session_or, callback_url, std::cout);
    if (!fetch_status.ok()) {
      std::cerr << "Failed to fetch OpenAI OAuth token: " << fetch_status.message() << std::endl;
      return 1;
    }
    std::cout << "Next: std_slop --openai_oauth" << std::endl;
    return 0;
  }

  if (fetch_openai_oauth_device_token) {
    std::shared_ptr<slop::OAuthHandler> bootstrap_oauth_handler;
    configure_openai_oauth_handler(&bootstrap_oauth_handler);
    auto start_or = bootstrap_oauth_handler->StartOpenAiDeviceAuthorization();
    if (!start_or.ok()) {
      std::cerr << "Failed to start OpenAI OAuth device flow: " << start_or.status().message() << std::endl;
      return 1;
    }
    std::cout << "OpenAI OAuth device flow\n"
              << "1. Open: " << start_or->verification_uri << "\n"
              << "2. Enter code: " << start_or->user_code << "\n"
              << "Waiting for authorization...\n";
    auto fetch_status = bootstrap_oauth_handler->FetchOpenAiDeviceToken(*start_or, std::cout);
    if (!fetch_status.ok()) {
      std::cerr << "Failed to fetch OpenAI OAuth token: " << fetch_status.message() << std::endl;
      return 1;
    }
    std::cout << "Next: std_slop --openai_oauth" << std::endl;
    return 0;
  }

  slop::Orchestrator::Builder builder(&db, &http_client);

  std::shared_ptr<slop::OAuthHandler> oauth_handler;
  std::unique_ptr<slop::Orchestrator> orchestrator;

  std::string google_key = absl::GetFlag(FLAGS_google_api_key);

  std::string openai_key = absl::GetFlag(FLAGS_openai_api_key);

  std::string openai_base_url = absl::GetFlag(FLAGS_openai_base_url);

  if (!openai_oauth && google_key.empty() && openai_key.empty()) {
    std::cerr << MissingAuthenticationMessage() << std::endl;
    std::cerr << absl::ProgramUsageMessage() << std::endl;
    return 1;
  }

  std::string model = absl::GetFlag(FLAGS_model);

  if (openai_oauth || !openai_key.empty()) {
    const bool openai_responses = openai_oauth || use_responses;
    std::string resolved_openai_base_url;
    if (openai_oauth) {
      resolved_openai_base_url = slop::kOpenAiChatGptCodexBaseUrl;
      if (!openai_base_url.empty()) {
        std::cout << "--openai_base_url ignored in --openai_oauth mode; using " << slop::kOpenAiChatGptCodexBaseUrl
                  << "." << std::endl;
      }
    } else {
      resolved_openai_base_url = !openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl;
    }
    builder.WithProvider(slop::Orchestrator::Provider::OPENAI)
        .WithModel(!model.empty() ? model : "gpt-5.4-mini:high")
        .WithBaseUrl(resolved_openai_base_url)
        .WithOpenAiApiStyle(openai_responses ? slop::Orchestrator::OpenAiApiStyle::RESPONSES
                                             : slop::Orchestrator::OpenAiApiStyle::CHAT_COMPLETIONS);
  } else {  // gemini API key
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
        .WithModel(!model.empty() ? model : "gemini-3-flash-preview");
  }

  auto orchestrator_or = builder.Build();
  if (!orchestrator_or.ok()) {
    std::cerr << "Failed to build orchestrator: " << orchestrator_or.status().message() << std::endl;
    return 1;
  }
  orchestrator = std::move(*orchestrator_or);

  if (openai_oauth) {
    configure_openai_oauth_handler(&oauth_handler);
    auto token_or = oauth_handler->GetValidToken();
    if (!token_or.ok()) {
      if (absl::IsUnauthenticated(token_or.status()) || absl::IsNotFound(token_or.status())) {
        std::cout << "OpenAI OAuth token path: " << oauth_handler->GetTokenPath() << std::endl;
        std::cout << "OpenAI OAuth: " << token_or.status().message() << std::endl;
        return 1;
      }
    }
  }

  auto tool_executor_or = slop::ToolExecutor::Create(&db);
  if (!tool_executor_or.ok()) {
    std::cerr << "Failed to initialize tool executor: " << tool_executor_or.status().message() << std::endl;
    return 1;
  }
  auto tool_executor = std::move(*tool_executor_or);

  auto dispatcher = std::make_unique<slop::ToolDispatcher>(
      [&tool_executor](const std::string& name, const nlohmann::json& args,
                       std::shared_ptr<slop::CancellationRequest> cancellation) -> absl::StatusOr<std::string> {
        return tool_executor->Execute(name, args, cancellation);
      });
  tool_executor->SetDispatcher(std::move(dispatcher));

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
  tool_executor->SetSessionId(session_id);

  std::vector<std::string> active_skills = tool_executor->GetActiveSkills();

  slop::InteractionEngine engine(db, *orchestrator, cmd_handler, *tool_executor->dispatcher(), *tool_executor,
                                 http_client, oauth_handler);
  slop::InteractionEngine::Config engine_config;
  engine_config.google_api_key = google_key;
  engine_config.openai_api_key = openai_key;
  engine_config.openai_base_url = openai_oauth ? slop::kOpenAiChatGptCodexBaseUrl : openai_base_url;
  engine_config.openai_oauth = openai_oauth;
  engine_config.use_responses = openai_oauth || use_responses;

  auto llm_query_invoker = [&engine, engine_config](const std::string& query,
                                                     const std::vector<std::string>& skills,
                                                     const slop::LlmQueryOptions& options)
      -> absl::StatusOr<std::string> {
    slop::InteractionEngine::QueryOptions query_options;
    query_options.session_id = options.session_id;
    query_options.skill = options.skill;
    query_options.context_window = options.context_window;
    query_options.execution_scope = options.execution_scope == slop::LlmQueryOptions::ExecutionScope::kSubquery
                                        ? slop::InteractionEngine::QueryOptions::ExecutionScope::kSubquery
                                        : slop::InteractionEngine::QueryOptions::ExecutionScope::kRoot;
    query_options.execution_depth = options.execution_depth;
    return engine.Query(query, engine_config, skills, query_options);
  };

  tool_executor->RegisterTool("llm_query",
                              [llm_query_invoker, active_skills](
                                  const nlohmann::json& args,
                                  std::shared_ptr<slop::CancellationRequest>) -> absl::StatusOr<std::string> {
                                auto query = slop::json_get<std::string>(args, "query");
                                if (!query) {
                                  return absl::InvalidArgumentError("Missing 'query' argument");
                                }
                                slop::LlmQueryOptions options;
                                options.session_id = "query";
                                options.execution_scope = slop::LlmQueryOptions::ExecutionScope::kRoot;
                                options.execution_depth = 0;
                                return llm_query_invoker(*query, active_skills, options);
                              });

  auto register_status = slop::ReconcileLlmSpecializationTools(&db, llm_specializations);
  if (!register_status.ok()) {
    std::cerr << "Failed to reconcile llm tool specializations: " << register_status.message() << std::endl;
    return 1;
  }

  register_status =
      slop::RegisterLlmSpecializationHandlers(tool_executor.get(), llm_specializations, active_skills, llm_query_invoker);
  if (!register_status.ok()) {
    std::cerr << "Failed to register llm tool specializations: " << register_status.message() << std::endl;
    return 1;
  }

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
