#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>
#include <iomanip>
#include <chrono>
#include "database.h"
#include "orchestrator.h"
#include "http_client.h"
#include "tool_executor.h"
#include "command_handler.h"
#include "ui.h"
#include "oauth_handler.h"
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
ABSL_FLAG(bool, google_oauth, false, "Use Google OAuth for authentication (internal only)");
ABSL_FLAG(std::string, project, "", "Set Google Cloud Project ID for OAuth mode");
ABSL_FLAG(std::string, model, "", "Model name (overrides GEMINI_MODEL or OPENAI_MODEL env vars)");
ABSL_FLAG(std::string, google_api_key, "", "Google API key (overrides GOOGLE_API_KEY env var)");
ABSL_FLAG(std::string, openai_api_key, "", "OpenAI API key (overrides OPENAI_API_KEY env var)");
ABSL_FLAG(std::string, openai_base_url, "", "OpenAI Base URL (overrides OPENAI_BASE_URL env var)");

void ShowHelp() {
  std::cout << "std::slop - The SQL-backed LLM CLI\n\n"
            << "Usage:\n"
            << "  std_slop [session_id] [options]\n\n"
            << "Options:\n"
            << "  Use --helpfull to see all available command-line flags.\n\n"
            << "Slash Commands:\n"
            << "  /message list [N]      List last N messages\n"
            << "  /message show <GID>    View full content of a group\n"
            << "  /message remove <GID>  Delete a message group\n"
            << "  /undo                  Remove last message and rebuild context\n"
            << "  /context               Show context status and assembled prompt\n"
            << "  /context window <N>    Set context to a rolling window of last N groups (0 for full)\n"
            << "  /context rebuild       Rebuild session state from conversation history\n"
            << "  /window <N>            Alias for /context window <N>\n"
            << "  /session               List all unique session names in the DB\n"
            << "  /session <name>        Switch to or create a new session named <name>\n"
            << "  /session remove <name> Delete a session and all its data\n"
            << "  /skill list            List all available skills\n"
            << "  /skill show <ID|Name>  Display the details of a skill\n"
            << "  /skill activate <ID|Name> Set active skill\n"
            << "  /skill deactivate <ID|Name> Disable active skill\n"
            << "  /skill add             Create new skill\n"
            << "  /skill edit <ID|Name>  Modify existing skill\n"
            << "  /skill delete <ID|Name> Remove skill\n"
            << "  /tool list             List available tools\n"
            << "  /tool show <name>      Show tool details\n"
            << "  /prompt-ledger [on|off] Toggle git-based prompt tracking\n"
            << "  /prompt-ledger <GID>   Show git diff for a specific interaction\n"
            << "  /stats /usage          Show session usage statistics\n"
            << "  /schema                Show current database schema\n"
            << "  /models                List available models\n"
            << "  /model <name>          Change active model\n"
            << "  /throttle [N]          Set/show request throttle\n"
            << "  /exec <command>        Execute shell command\n"
            << "  /edit                  Open last input in EDITOR\n"
            << "  /exit /quit            Exit the program\n"
            << std::endl;
}

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage("std::slop - The SQL-backed LLM CLI\nUsage: std_slop [session_id] [options]");
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
    std::cerr << "Error: No authentication method configured. Set GOOGLE_API_KEY, OPENAI_API_KEY, or use flags." << std::endl;
    return 1;
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
    orchestrator.SetModel(!model.empty() ? model : "gemini-3-flash-preview");
    base_url = "https://cloudcode-pa.googleapis.com/v1internal";
    orchestrator.SetGcaMode(true);
  } else if (!openai_key.empty()) {
    provider = slop::Orchestrator::Provider::OPENAI;
    orchestrator.SetModel(!model.empty() ? model : "gpt-4o");
    base_url = !openai_base_url.empty() ? openai_base_url : "https://api.openai.com/v1";
    headers.push_back("Authorization: Bearer " + openai_key);
  } else {
    provider = slop::Orchestrator::Provider::GEMINI;
    orchestrator.SetModel(!model.empty() ? model : "gemini-2.0-flash");
    base_url = "https://generativelanguage.googleapis.com/v1beta";
  }
  orchestrator.SetProvider(provider);

  std::unique_ptr<slop::OAuthHandler> oauth_handler;
  if (google_auth) {
    oauth_handler = std::make_unique<slop::OAuthHandler>(&http_client);
    if (!manual_project_id.empty()) {
      oauth_handler->SetProjectId(manual_project_id);
    }
    oauth_handler->SetEnabled(true);
    auto token_or = oauth_handler->GetValidToken();
    if (!token_or.ok()) {
      std::cerr << "OAuth Error: " << token_or.status().message() << std::endl;
    }
    auto proj_or = oauth_handler->GetProjectId();
    if (proj_or.ok()) {
        orchestrator.SetProjectId(*proj_or);
    }
  }

  slop::ToolExecutor tool_executor(&db);
  slop::CommandHandler cmd_handler(&db, &orchestrator, oauth_handler.get(), google_key, openai_key);
  std::vector<std::string> active_skills;

  slop::SetupTerminal();
  
  std::cout << "std::slop - Session: " << session_id << " [" << orchestrator.GetModel() << "]" << std::endl;
  if (google_auth) std::cout << "Mode: Google OAuth (Internal)" << std::endl;
  else if (!openai_key.empty()) std::cout << "Mode: OpenAI" << std::endl;
  else std::cout << "Mode: Google Gemini (API Key)" << std::endl;
  std::cout << "Type /help for commands.\n" << std::endl;

  while (true) {
    std::string prompt_str = "";
    auto context_settings = db.GetContextSettings(session_id);
    if (context_settings.ok()) {
      if (context_settings->size > 0) {
          absl::StrAppend(&prompt_str, "[WINDOW: ", context_settings->size, "]");
      } else if (context_settings->size == 0) {
          absl::StrAppend(&prompt_str, "[FULL]");
      } else {
          absl::StrAppend(&prompt_str, "[NONE]");
      }
    }
    if (!active_skills.empty()) {
      absl::StrAppend(&prompt_str, " [", absl::StrJoin(active_skills, ", "), "]");
    }

    auto pl_val = db.GetGlobalSetting("prompt_ledger_enabled");
    if (pl_val.ok() && *pl_val == "true") {
        absl::StrAppend(&prompt_str, " [pl]");
    }

    absl::StrAppend(&prompt_str, " [", orchestrator.GetModel(), "] User> ");

    std::string input = slop::ReadLine(prompt_str.c_str(), session_id);
    if (input.empty()) continue;

    auto res = cmd_handler.Handle(input, session_id, active_skills, ShowHelp, orchestrator.GetLastSelectedGroups());
    if (res == slop::CommandHandler::Result::HANDLED) {
      if (input == "/exit" || input == "/quit") break;
      continue;
    }

    if (input[0] == '/') {
        std::cerr << "Unknown command: " << input << std::endl;
        continue;
    }

    auto prompt_or = orchestrator.AssemblePrompt(session_id, active_skills);
    if (!prompt_or.ok()) {
      std::cerr << "Error assembling prompt: " << prompt_or.status().message() << std::endl;
      continue;
    }

    std::string group_id = slop::GenerateGroupId();
    auto status = db.AppendMessage(session_id, "user", input, "", "completed", group_id);
    if (!status.ok()) {
        std::cerr << "Error storing message: " << status.message() << std::endl;
        continue;
    }

    // Refresh prompt with the new user message
    prompt_or = orchestrator.AssemblePrompt(session_id, active_skills);

    status = orchestrator.ProcessResponse(session_id, prompt_or->dump(), group_id);
    if (!status.ok()) {
      std::cerr << "Error: " << status.message() << std::endl;
    }
    
    orchestrator.FinalizeInteraction(session_id, group_id);
  }

  return 0;
}
