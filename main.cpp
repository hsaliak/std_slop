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
#include "constants.h"
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
         "  /context               Show context status and assembled prompt\n" 
         "  /context window <N>    Set context to a rolling window of last N groups (0 for full)\n" 
         "  /context rebuild       Rebuild session state from conversation history\n" 
         "  /window <N>            Alias for /context window <N>\n" 
         "  /session               List all unique session names in the DB\n" 
         "  /session <name>        Switch to or create a new session named <name>\n" 
         "  /session remove <name> Delete a session and all its data\n" 
         "  /skill list            List all available skills\n" 
         "  /skill show <ID|Name>  Display the details of a skill\n" 
         "  /skill activate <ID|Name> Set active skill\n" 
         "  /skill deactivate <ID|Name> Disable active skill\n" 
         "  /skill add             Create new skill\n" 
         "  /skill edit <ID|Name>  Modify existing skill\n" 
         "  /skill delete <ID|Name> Remove skill\n" 
         "  /tool list             List available tools\n" 
         "  /tool show <name>      Show tool details\n" 
         "  /stats /usage          Show session usage statistics\n" 
         "  /schema                Show current database schema\n" 
         "  /models                List available models\n" 
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
    orchestrator.SetModel(!model.empty() ? model : "gemini-2.0-flash");
    base_url = absl::StrCat(slop::kCloudCodeBaseUrl, "/v1internal");
    orchestrator.SetGcaMode(true);
  } else if (!openai_key.empty()) {
    provider = slop::Orchestrator::Provider::OPENAI;
    orchestrator.SetModel(!model.empty() ? model : "gpt-4o");
    base_url = !openai_base_url.empty() ? openai_base_url : slop::kOpenAIBaseUrl;
    headers.push_back("Authorization: Bearer " + openai_key);
  } else {
    provider = slop::Orchestrator::Provider::GEMINI;
    orchestrator.SetModel(!model.empty() ? model : "gemini-2.0-flash");
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
        orchestrator.SetProjectId(*proj_or);
    }
  }

  slop::ToolExecutor tool_executor(&db);
  slop::CommandHandler cmd_handler(&db, &orchestrator, oauth_handler.get(), google_key, openai_key);
  std::vector<std::string> active_skills;

  slop::SetupTerminal();
  
  std::cout << "std::slop - Session: " << session_id << " [" << orchestrator.GetModel() << "]" << std::endl;
  if (google_auth) std::cout << "Mode: Google OAuth" << std::endl;
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
    absl::StrAppend(&prompt_str, " [", orchestrator.GetModel(), "] User> ");

    std::string input = slop::ReadLine(prompt_str.c_str(), session_id);
    if (input.empty()) continue;

    auto res = cmd_handler.Handle(input, session_id, active_skills, ShowHelp, orchestrator.GetLastSelectedGroups());
    if (res == slop::CommandHandler::Result::HANDLED) {
      if (input == "/exit" || input == "/quit") break;
      continue;
    }

    if (res == slop::CommandHandler::Result::UNKNOWN) continue;

    std::string group_id = std::to_string(absl::ToUnixNanos(absl::Now()));
    (void)db.AppendMessage(session_id, "user", input, "", "completed", group_id);

    while (true) {
      auto prompt_or = orchestrator.AssemblePrompt(session_id, active_skills);
      if (!prompt_or.ok()) {
        std::cerr << "Prompt Assembly Error: " << prompt_or.status().message() << std::endl;
        break;
      }

      int context_tokens = orchestrator.CountTokens(*prompt_or);
      std::vector<std::string> current_headers = headers;
      std::string url = base_url;

      if (provider == slop::Orchestrator::Provider::GEMINI) {
        if (orchestrator.GetProvider() == slop::Orchestrator::Provider::GEMINI && oauth_handler && oauth_handler->IsEnabled()) {
          if (absl::StrContains(url, "v1internal")) {
              url = absl::StrCat(url, ":generateContent");
          } else {
              url = absl::StrCat(url, "/models/", orchestrator.GetModel(), ":generateContent");
          }
          auto token_or = oauth_handler->GetValidToken();
          if (token_or.ok()) {
              current_headers.push_back("Authorization: Bearer " + *token_or);
          } else {
              std::cerr << "OAuth Error: " << token_or.status().message() << std::endl;
              break;
          }
        }
        else {
          url = absl::StrCat(url, "/models/", orchestrator.GetModel(), ":generateContent?key=", !google_key.empty() ? google_key : "");
        }
      } else {
        url = absl::StrCat(url, "/chat/completions");
      }

      // Thinking... UI
      std::string skill_suffix;
      if (!active_skills.empty()) {
          skill_suffix = " [" + absl::StrJoin(active_skills, ", ") + "]";
      }
      std::cout << "[context: " << context_tokens << " tokens] Thinking" << skill_suffix << "... " << std::flush;

      absl::StatusOr<std::string> response_or;
      int retry_count = 0;
      int max_retries = 5;
      long backoff_ms = 2000;

      while (retry_count <= max_retries) {
        response_or = http_client.Post(url, prompt_or->dump(), current_headers);
        if (response_or.ok()) break;

        if (absl::IsResourceExhausted(response_or.status())) {
          if (retry_count < max_retries) {
            std::cout << "\rRate limit hit. Retrying in " << (backoff_ms / 1000) << "s... (Attempt " << (retry_count + 1) << "/" << max_retries << ")" << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms *= 2;
            retry_count++;
            std::cout << "\rThinking" << skill_suffix << "... " << std::string(50, ' ') << "\rThinking" << skill_suffix << "... " << std::flush;
            continue;
          }
        }
        break;
      }

      // Clear the "Thinking..." line
      std::cout << "\r" << std::string(100, ' ') << "\r" << std::flush;

      if (!response_or.ok()) {
        if (absl::IsInternal(response_or.status()) && absl::StrContains(response_or.status().message(), "HTTP error 403")) {
            std::cerr << "\nHTTP Error 403: Forbidden. Your OAuth token may have insufficient scopes.\n" 
                      << "Please re-run the authentication script:\n" 
                      << "  ./slop_auth.sh\n" << std::endl;
        } else {
            std::cerr << "HTTP Error: " << response_or.status().message() << " (URL: " << url << ")" << std::endl;
        }
        break;
      }

      auto process_status = orchestrator.ProcessResponse(session_id, *response_or, group_id);
      if (!process_status.ok()) {
        std::cerr << "Response Processing Error: " << process_status.message() << std::endl;
        break;
      }

      auto history_or = db.GetConversationHistory(session_id);
      if (!history_or.ok() || history_or->empty()) break;
      const auto& last_msg = history_or->back();

      if (last_msg.status == "completed") {
        std::string skill_str;
        if (!active_skills.empty()) {
            skill_str = " (" + absl::StrJoin(active_skills, ", ") + ")";
        }
        std::cout << "\n[Assistant" << skill_str << "]: " << last_msg.content << "\n" << std::endl;
        break; 
      } else if (last_msg.status == "tool_call") {
        auto tc_or = orchestrator.ParseToolCall(last_msg);
        if (tc_or.ok()) {
          std::cout << "\n[Tool Call]: " << tc_or->name << "(" << tc_or->args.dump() << ")" << std::endl;
          auto tool_res = tool_executor.Execute(tc_or->name, tc_or->args);
          std::string display_res = tool_res.ok() ? *tool_res : "Error: " + std::string(tool_res.status().message());
          std::cout << "[Tool Result]: " << (display_res.size() > 500 ? display_res.substr(0, 500) + "..." : display_res) << "\n" << std::endl;
          
          std::string logged_res = display_res;

          if (provider == slop::Orchestrator::Provider::GEMINI) {
              nlohmann::json tool_msg = {
                  {"functionResponse", {
                      {"name", tc_or->name},
                      {"response", {{"content", logged_res}}}
                  }}
              };
              (void)db.AppendMessage(session_id, "tool", tool_msg.dump(), tc_or->name, "completed", group_id);
          } else {
              nlohmann::json tool_msg = {{"content", logged_res}};
              (void)db.AppendMessage(session_id, "tool", tool_msg.dump(), tc_or->id + "|" + tc_or->name, "completed", group_id);
          }

          // Throttle agentic loop
          if (orchestrator.GetThrottle() > 0) {
              std::this_thread::sleep_for(std::chrono::seconds(orchestrator.GetThrottle()));
          }
        } else {
          std::cerr << "Tool Parse Error: " << tc_or.status().message() << std::endl;
          break;
        }
      }
    }
  }

  return 0;
}