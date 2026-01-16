#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>
#include <future>
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
            << "  /message view <GID>    View full content of a group\n"
            << "  /message remove <GID>  Delete a message group\n"
            << "  /message drop <GID>    Mark a message group as 'dropped' (ignored by context)\n"
            << "  /context show          Show currently active conversation context\n"
            << "  /context drop          Drop all messages in the current session from context\n"
            << "  /context build [N]     Re-enable the last N groups into context\n"
            << "  /context-mode fts <N>  Use FTS-ranked context (BM25 + Recency) with top N groups\n"
            << "  /context-mode full     Use all messages in the session (default)\n"
            << "  /skill <subcommand>    Manage skills:\n"
            << "      list                 List all available skills\n"
            << "      activate <ID|Name>   Set the active skill for the current session\n"
            << "      deactivate <ID|Name> Disable an active skill\n"
            << "      add                  Create a new skill using your $EDITOR\n"
            << "      edit <ID|Name>       Modify an existing skill using your $EDITOR\n"
            << "      view <ID|Name>       Display the details of a skill\n"
            << "      delete <ID|Name>     Remove a skill from the database\n"
            << "  /sessions              List all unique session IDs in the DB\n"
            << "  /switch <ID>           Switch to a different session\n"
            << "  /undo                  Drop the last message group (user + assistant response)\n"
            << "  /stats /usage          Show session usage statistics\n"
            << "  /schema                Show current database schema\n"
            << "  /models                List available models from the provider\n"
            << "  /model <name>          Change the active model\n"
            << "  /throttle [N]          Set or show request throttle (seconds) for agentic loops\n"
            << "  /exec <command>        Execute a shell command and pipe through a pager\n"
            << "  /edit                  Open the last input in your EDITOR\n"
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
  
  std::cout << "std::slop - Session: " << session_id 
            << " | Model: " << orchestrator.GetModel() 
            << " (Provider: " << (provider == slop::Orchestrator::Provider::GEMINI ? "Gemini" : "OpenAI") << ")" << std::endl;
  std::cout << "Type /help for commands, or just start chatting." << std::endl;

  while (true) {
    std::string prompt_str;
    auto context_settings = db.GetContextSettings(session_id);
    if (context_settings.ok()) {
      if (context_settings->mode == slop::Database::ContextMode::FTS_RANKED) {
        absl::StrAppend(&prompt_str, "[FTS: ", context_settings->size, "]");
      } else {
        absl::StrAppend(&prompt_str, "[FULL]");
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
        std::cerr << "Prompt Error: " << prompt_or.status().message() << std::endl;
        break;
      }

      std::string url = base_url;
      if (!url.empty() && url.back() == '/') url.pop_back();

      std::vector<std::string> current_headers = headers;
      std::string count_api_key = (provider == slop::Orchestrator::Provider::GEMINI) ? google_key : openai_key;

      if (provider == slop::Orchestrator::Provider::GEMINI) {
        if (google_auth) {
          url = absl::StrCat(url, ":generateContent");
          auto token_or = oauth_handler->GetValidToken();
          if (token_or.ok()) {
              current_headers.push_back("Authorization: Bearer " + *token_or);
              count_api_key = *token_or;
          }
        }
        else {
          url = absl::StrCat(url, "/models/", orchestrator.GetModel(), ":generateContent?key=", !google_key.empty() ? google_key : "");
        }
      } else {
        url = absl::StrCat(url, "/chat/completions");
      }

      // Thinking... UI with Timer
      auto start_time = absl::Now();
      std::string skill_suffix;
      if (!active_skills.empty()) {
          skill_suffix = " [" + absl::StrJoin(active_skills, ", ") + "]";
      }
      std::cout << "Thinking" << skill_suffix << "... " << std::flush;
      
      auto token_count_future = std::async(std::launch::async, [&]() {
          return orchestrator.CountTokens(*prompt_or, count_api_key);
      });

      auto future = std::async(std::launch::async, [&]() {
        return http_client.Post(url, prompt_or->dump(), current_headers);
      });

      int context_tokens = -1;
      while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (context_tokens == -1 && token_count_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto count_res = token_count_future.get();
            if (count_res.ok()) context_tokens = *count_res;
            else context_tokens = 0;
        }

        double elapsed = absl::ToDoubleSeconds(absl::Now() - start_time);
        std::string token_info = (context_tokens >= 0) ? "[context: " + std::to_string(context_tokens) + " tokens] " : "";
        std::cout << "\r" << token_info << "Thinking" << skill_suffix << "... (" << std::fixed << std::setprecision(1) << elapsed << "s)" << std::flush;
      }

      auto response_or = future.get();
      // Clear the "Thinking..." line
      std::cout << "\r" << std::string(100, ' ') << "\r" << std::flush;

      if (!response_or.ok()) {
        std::cerr << "HTTP Error: " << response_or.status().message() << " (URL: " << url << ")" << std::endl;
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
          
          if (provider == slop::Orchestrator::Provider::GEMINI) {
              nlohmann::json tool_msg = {
                  {"functionResponse", {
                      {"name", tc_or->name},
                      {"response", {{"content", display_res}}}
                  }}
              };
              (void)db.AppendMessage(session_id, "tool", tool_msg.dump(), tc_or->name, "completed", group_id);
          } else {
              nlohmann::json tool_msg = {{"content", display_res}};
              (void)db.AppendMessage(session_id, "tool", tool_msg.dump(), tc_or->id + "|" + tc_or->name, "completed", group_id);
          }

          // Throttle agentic loop
          if (orchestrator.GetThrottle() > 0) {
              std::this_thread::sleep_for(std::chrono::seconds(orchestrator.GetThrottle()));
          }

        } else {
            std::cerr << "Failed to parse tool call: " << tc_or.status().message() << std::endl;
            break;
        }
      }
    }
  }

  return 0;
}
