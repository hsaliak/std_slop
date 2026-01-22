#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include "database.h"
#include "orchestrator.h"
#include "http_client.h"
#include "tool_executor.h"
#include "command_handler.h"
#include "ui.h"
#include "oauth_handler.h"
#include "constants.h"
#include "color.h"
#include "absl/strings/strip.h"
#include "absl/strings/str_split.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"

ABSL_FLAG(std::string, db, "slop.db", "Path to SQLite database");
ABSL_FLAG(bool, google_oauth, false, "Use Google OAuth for authentication");
ABSL_FLAG(std::string, project, "", "Set Google Cloud Project ID for OAuth mode");
ABSL_FLAG(std::string, model, "", "Model name (overrides GEMINI_MODEL or OPENAI_MODEL env vars)");
ABSL_FLAG(std::string, google_api_key, "", "Google API key (overrides GOOGLE_API_KEY env var)");
ABSL_FLAG(std::string, openai_api_key, "", "OpenAI API key (overrides OPENAI_API_KEY env var)");
ABSL_FLAG(std::string, openai_base_url, "", "OpenAI Base URL (overrides OPENAI_BASE_URL env var)");
ABSL_FLAG(bool, strip_reasoning, false, "Strip reasoning from OpenAI-compatible API responses (Recommended when using newer models via OpenRouter to improve response speed and focus)");

std::string GetHelpText() {
  return "std::slop - The SQL-backed LLM CLI\n\n"
         "Usage:\n"
         "  std_slop [session_id] [options]\n\n"
         "Options:\n"
         "  Use --helpfull to see all available command-line flags.\n\n"
         "Slash Commands:\n"
         "  /message list [N]      List last N messages\n"
         "  /message show <GID>    View full content of a group\n"
         "  /message remove <GID>  Delete a message group\n"
         "  /undo                  Remove last message and rebuild context\n"
         "  /context show          Show context status and assembled prompt\n"
         "  /context window <N>    Set context to a rolling window of last N groups (0 for full)\n"
         "  /context rebuild       Rebuild session state from conversation history\n"
         "  /session list          List all unique session names in the DB\n"
         "  /session activate <name> Switch to or create a new session named <name>\n"
         "  /session remove <name> Delete a session and all its data\n"
         "  /session clear         Clear all history and state for current session\n"
         "  /skill list            List all available skills\n"
         "  /skill activate <ID|Name> Set active skill\n"
         "  /skill deactivate <ID|Name> Disable active skill\n"
         "  /skill add             Create new skill\n"
         "  /skill edit <ID|Name>  Modify existing skill\n"
         "  /skill delete <ID|Name> Remove skill\n"
         "  /todo                  Manage your personal task list\n"
         "  /todo list [group]     List todos (optionally by group)\n"
         "  /todo add <group> <desc> Add a new todo to the specified group\n"
         "  /todo edit <group> <id> <desc> Edit the description of a todo by its ID within a group\n"
         "  /todo complete <group> <id> Mark a todo as complete by its ID within a group\n"
         "  /todo drop <group>     Delete all todos in the specified group\n"
         "  /tool list             List available tools\n"
         "  /tool show <name>      Show tool details\n"
         "  /stats /usage          Show session usage statistics\n"
         "  /schema                Show current database schema\n"
         "  /models [filter]       List available models\n"
         "  /model <name>          Change active model\n"
         "  /throttle [N]          Set/show request throttle\n"
         "  /exec <command>        Execute shell command\n"
         "  /edit                  Open last input in EDITOR\n"
         "  /exit /quit            Exit the program\n";
}

void ShowHelp() {
  std::cout << GetHelpText() << std::endl;
}

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(GetHelpText());
  std::vector<char*> positional_args = absl::ParseCommandLine(argc, argv);

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
  std::string openai_base_url = !flag_openai_base_url.empty() ? flag_openai_base_url : (env_openai_base_url ? env_openai_base_url : "");
  
  std::string model = flag_model;
  if (model.empty()) {
      const char* env_gemini = std::getenv("GEMINI_MODEL");
      const char* env_openai = std::getenv("OPENAI_MODEL");
      if (env_gemini) model = env_gemini;
      else if (env_openai) model = env_openai;
  }

  if (!google_auth && google_key.empty() && openai_key.empty()) {
    google_auth = true;
    std::cout << "No API keys found. Defaulting to Google OAuth mode." << std::endl;
  }

  slop::Database db;
  auto status = db.Init(db_path);
  if (!status.ok()) {
    std::cerr << "Database Error: " << status.message() << std::endl;
    return 1;
  }

  slop::HttpClient http_client;
  slop::Orchestrator::Builder builder(&db, &http_client);
  builder.WithStripReasoning(absl::GetFlag(FLAGS_strip_reasoning));

  if (google_auth) { // google OAuth
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
           .WithModel(!model.empty() ? model : "gemini-2.5-flash")
           .WithBaseUrl(absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal"))
           .WithGcaMode(true);
  } else if (!openai_key.empty()) { // openAI API key
    builder.WithProvider(slop::Orchestrator::Provider::OPENAI)
           .WithModel(!model.empty() ? model : "gpt-4o")
           .WithBaseUrl(!openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl);
  } else { // gemini API key
    builder.WithProvider(slop::Orchestrator::Provider::GEMINI)
           .WithModel(!model.empty() ? model : "gemini-2.5-flash")
           .WithBaseUrl(slop::kPublicGeminiBaseUrl);
  }

  auto orchestrator = builder.Build();

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

  slop::ToolExecutor tool_executor(&db);
  slop::CommandHandler cmd_handler(&db, orchestrator.get(), oauth_handler.get(), google_key, openai_key);
  std::vector<std::string> active_skills;

  slop::ShowBanner();
  std::cout << slop::Colorize("std::slop", "", ansi::Cyan) << " - Session: " << session_id << " (" << orchestrator->GetModel() << ")" << std::endl;
  std::cout << "Type /help for slash commands." << std::endl;

  (void)slop::DisplayHistory(db, session_id, 20);
  (void)orchestrator->RebuildContext(session_id);

  while (true) {
    auto settings_or = db.GetContextSettings(session_id);
    int window_size = settings_or.ok() ? settings_or->size : 0;
    std::string model_name = orchestrator->GetModel();
    std::string persona = active_skills.empty() ? "default" : absl::StrJoin(active_skills, ",");
    std::string window_str = (window_size == 0) ? "all" : std::to_string(window_size);
    std::string modeline = absl::StrCat("std::slop<window<", window_str, ">, ", model_name, ", ", persona, ", ", session_id, ">");

    std::string input = slop::ReadLine(modeline);
    if (input == "/exit" || input == "/quit") break;
    if (input.empty()) continue;

    auto res = cmd_handler.Handle(input, session_id, active_skills, ShowHelp, orchestrator->GetLastSelectedGroups());
    if (res == slop::CommandHandler::Result::HANDLED || res == slop::CommandHandler::Result::UNKNOWN) {
        continue;
    }

    // Execute interaction
    std::string group_id = std::to_string(absl::ToUnixNanos(absl::Now()));
    (void)db.AppendMessage(session_id, "user", input, "", "completed", group_id);

    while (true) {
        auto prompt_or = orchestrator->AssemblePrompt(session_id, active_skills);
        if (!prompt_or.ok()) {
            std::cerr << "Prompt Error: " << prompt_or.status().message() << std::endl;
            break;
        }

        int context_tokens = orchestrator->CountTokens(*prompt_or);
        std::cout << "[context: " << context_tokens << " tokens] Thinking...\n " << std::flush;

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
            url = absl::StrCat(slop::kPublicGeminiBaseUrl, "/models/", orchestrator->GetModel(), ":generateContent?key=", google_key);
        }

        auto resp_or = http_client.Post(url, prompt_or->dump(), headers);
        if (!resp_or.ok()) {
            std::cerr << "HTTP Error: " << resp_or.status().message() << std::endl;
            if (google_auth && (absl::IsUnauthenticated(resp_or.status()) || absl::IsPermissionDenied(resp_or.status()))) {
                std::cout << "Refreshing OAuth token..." << std::endl;
                (void)oauth_handler->GetValidToken();
            }
            break;
        }

        auto status = orchestrator->ProcessResponse(session_id, *resp_or, group_id);
        if (!status.ok()) {
            std::cerr << "Process Error: " << status.message() << std::endl;
            break;
        }

        auto history_or = db.GetMessagesByGroups({group_id});
        if (!history_or.ok() || history_or->empty()) break;

        const auto& last_msg = history_or->back();
        if (last_msg.role == "assistant" && last_msg.status == "tool_call") {
            auto calls_or = orchestrator->ParseToolCalls(last_msg);
            if (calls_or.ok()) {
                for (const auto& call : *calls_or) {
                    slop::PrintToolCallMessage(call.name, call.args.dump());
                    auto result_or = tool_executor.Execute(call.name, call.args);
                    std::string result = result_or.ok() ? *result_or : absl::StrCat("Error: ", result_or.status().message());
                    slop::PrintToolResultMessage(result);
                    (void)db.AppendMessage(session_id, "tool", result, last_msg.tool_call_id, "completed", group_id, last_msg.parsing_strategy);
                }
                if (orchestrator->GetThrottle() > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(orchestrator->GetThrottle()));
                }
                continue; // Loop for next LLM turn
            }
        } else if (last_msg.role == "assistant") {
            slop::PrintAssistantMessage(last_msg.content);
        }
        break;
    }
  }

  return 0;
}
