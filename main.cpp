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
  slop::Orchestrator orchestrator(&db, &http_client);

  std::string base_url;
  slop::Orchestrator::Provider provider;
  std::vector<std::string> headers = {"Content-Type: application/json"};

  if (google_auth) {
    provider = slop::Orchestrator::Provider::GEMINI;
    orchestrator.SetModel(!model.empty() ? model : "gemini-2.5-flash");
    base_url = absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal");
    orchestrator.SetGcaMode(true);
  } else if (!openai_key.empty()) {
    provider = slop::Orchestrator::Provider::OPENAI;
    orchestrator.SetModel(!model.empty() ? model : "gpt-4o");
    base_url = !openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl;
    headers.push_back("Authorization: Bearer " + openai_key);
  } else {
    provider = slop::Orchestrator::Provider::GEMINI;
    orchestrator.SetModel(!model.empty() ? model : "gemini-2.5-flash");
    base_url = slop::kPublicGeminiBaseUrl;
  }
  orchestrator.SetProvider(provider);
  orchestrator.SetBaseUrl(base_url);

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
      } else {
          std::cerr << "OAuth Error: " << token_or.status().message() << std::endl;
      }
    }
    auto proj_or = oauth_handler->GetProjectId();
    if (proj_or.ok()) {
      headers.push_back("x-goog-user-project: " + std::string(*proj_or));
    }
    orchestrator.SetOAuthHandler(oauth_handler.get());
  } else if (!google_key.empty()) {
    headers.push_back("x-api-key: " + google_key);
  }
  orchestrator.SetHeaders(headers);

  slop::ToolExecutor tool_executor(&db);
  slop::UI ui;
  slop::CommandHandler command_handler(&orchestrator, &tool_executor, &db, &ui, session_id);
  
  ui.Init();
  ui.SetMessageCallback([&](const std::string& message) {
    if (message == "/help") {
      ShowHelp();
      return true;
    } else if (message == "/exit" || message == "/quit") {
      ui.Stop();
      return true;
    } else {
      command_handler.HandleCommand(message);
      return true;
    }
  });

  ui.Run();
  return 0;
}