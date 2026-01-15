#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include "database.h"
#include "http_client.h"
#include "orchestrator.h"
#include "tool_executor.h"
#include "command_handler.h"
#include "ui.h"
#include "completion.h"
#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "absl/strings/strip.h"
#include <nlohmann/json.hpp>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

namespace sentinel { }

void ShowUsage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [session_id] [options]\n"
              << "Options:\n"
              << "  -h, --help    Show this help message\n"
              << "\nEnvironment Variables:\n"
              << "  GOOGLE_API_KEY      Your Gemini API key\n"
              << "  OPENAI_API_KEY      Your OpenAI-compatible API key\n"
              << std::endl;
}

void ShowHelp() {
    std::cout << "\n--- Attempt1 Help ---\n"
              << "Core Commands:\n"
              << "  /help                - Show this help message\n"
              << "  /edit                - Open $EDITOR to compose a long message\n"
              << "  /undo                - Revert the last interaction group\n"
              << "  /exit, /quit         - End the session\n"
              << "\nContext & Messages (Group-Based):\n"
              << "  /message list [N]    - List last N history entries by Group ID\n"
              << "  /message view [G]    - View message group G in $EDITOR\n"
              << "  /message remove [G]  - Permanently delete message group G\n"
              << "  /message drop [G]    - Hide message group G from LLM context\n"
              << "  /context show [N]    - Show last N messages of active history\n"
              << "  /context drop        - Hide all current messages from the LLM\n"
              << "  /context build [N]   - Reactivate last N message groups\n"
              << "  /low-context-mode on <N> | off - Limit context to last N turns\n"
              << "\nKnowledge & Skills:\n"
              << "  /skills              - List all available agent skills\n"
              << "  /skill activate <N>  - Switch to skill N\n"
              << "\nSystem & Sessions:\n"
              << "  /sessions            - List all conversation sessions\n"
              << "  /switch <ID>         - Switch to a different session ID\n"
              << "  /model               - List and switch the active LLM model\n"
              << "  /stats               - Show session message statistics\n"
              << "  /schema              - Show the internal database schema\n"
              << "-------------------------" << std::endl;
}

int main(int argc, char** argv) {
    const char* google_key = std::getenv("GOOGLE_API_KEY");
    const char* openai_key = std::getenv("OPENAI_API_KEY");
    const char* openai_base = std::getenv("OPENAI_BASE_URL");
    const char* env_model = std::getenv("GEMINI_MODEL");
    if (!env_model) env_model = std::getenv("OPENAI_MODEL");

    if (!google_key && !openai_key) {
        std::cerr << "Error: No API keys set (GOOGLE_API_KEY or OPENAI_API_KEY)." << std::endl;
        return 1;
    }

    std::string model_name;
    sentinel::Orchestrator::Provider provider;
    std::string base_url;
    std::vector<std::string> headers = {"Content-Type: application/json"};

    if (openai_key) {
        provider = sentinel::Orchestrator::Provider::OPENAI;
        model_name = env_model ? env_model : "gpt-4o";
        base_url = openai_base ? openai_base : "https://api.openai.com/v1";
        headers.push_back("Authorization: Bearer " + std::string(openai_key));
    } else {
        provider = sentinel::Orchestrator::Provider::GEMINI;
        model_name = env_model ? env_model : "gemini-3-flash-preview";
        base_url = "https://generativelanguage.googleapis.com/v1beta";
    }

    sentinel::Database db;
    if (!db.Init("sentinel.db").ok()) return 1;

    sentinel::HttpClient http_client;
    sentinel::Orchestrator orchestrator(&db, &http_client);
    orchestrator.SetProvider(provider);
    orchestrator.SetModel(model_name);
    sentinel::ToolExecutor tool_executor(&db);

    // Register default tools - NO BACKSLASHES in these raw strings!
    std::vector<sentinel::Database::Tool> default_tools = {
        {"read_file", "Read the contents of a file.", R"({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]})", true},
        {"write_file", "Write content to a file.", R"({"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]})", true},
        {"execute_bash", "Run a bash command.", R"({"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]})", true},
        {"search_code", "Search using FTS5.", R"({"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]})", true},
        {"index_directory", "Index files.", R"({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]})", true},
        {"query_db", "Query agent's DB.", R"({"type": "object", "properties": {"sql": {"type": "string"}}, "required": ["sql"]})", true}
    };
    for (const auto& t : default_tools) (void)db.RegisterTool(t);
    (void)db.RegisterSkill({0, "expert_coder", "C++ Expert", "You are an expert C++17 developer. Follow Google Style. No exceptions.", "[]"});
    (void)db.RegisterSkill({0, "chat", "Concise Chat", "You are an expert advanced LLM chat. Provide precise and concise answers. No fluff. You are NOT allowed to write any files.", "[]"});
    (void)db.RegisterSkill({0, "planner", "Feature Planner", "You are an expert software architect and planner. You excel at planning large features, breaking them down into small, digestible chunks for sequential execution, and recommending comprehensive tests. You are only allowed to write markdown files for plans.", "[]"});

    std::string session_id = (argc > 1) ? argv[1] : "default_session";
    std::vector<std::string> active_skills = {"chat"};

#ifdef HAVE_READLINE
    sentinel::InitCompletion("commands.json");
#endif

    std::cout << "Attempt1 Ready (" << (provider == sentinel::Orchestrator::Provider::OPENAI ? "OpenAI" : "Gemini") << "). Type '/help' for commands." << std::endl;

    sentinel::CommandHandler command_handler(&db);

    while (true) {
        std::string input;
        std::string persona = active_skills.empty() ? "none" : active_skills[0];
        
        std::string context_indicator = "[Full]";
        auto settings_or = db.GetContextSettings(session_id);
        if (settings_or.ok() && settings_or->mode == sentinel::Database::ContextMode::FTS_RANKED) {
            context_indicator = "[FTS: " + std::to_string(settings_or->size) + "]";
        }

        std::string prompt = "\n" + context_indicator + "[" + persona + "] User> ";
#ifdef HAVE_READLINE
        char* line = readline(prompt.c_str());
        if (!line) break;
        if (*line) add_history(line); 
        input = line; free(line);
#else
        std::cout << prompt; if (!std::getline(std::cin, input)) break;
#endif
        std::string trimmed = std::string(absl::StripAsciiWhitespace(input));
        if (trimmed == "/exit" || trimmed == "/quit") break;
        if (input.empty()) continue;

        if (input[0] == '/') {
            auto res = command_handler.Handle(input, session_id, active_skills, ShowHelp);
            if (res != sentinel::CommandHandler::Result::PROCEED_TO_LLM) continue;
        }

        std::string group_id = std::to_string(absl::GetCurrentTimeNanos());
        (void)db.AppendMessage(session_id, "user", input, "", "completed", group_id);

        bool needs_llm = true;
        while (needs_llm) {
            needs_llm = false;
            auto prompt_payload = orchestrator.AssemblePrompt(session_id, active_skills);
            if (!prompt_payload.ok()) break;

            std::cout << "Thinking..." << std::flush;
            std::string url = (provider == sentinel::Orchestrator::Provider::GEMINI)
                ? base_url + "/models/" + model_name + ":generateContent" + (google_key ? "?key=" + std::string(google_key) : "")
                : base_url + "/chat/completions";

            auto res = http_client.Post(url, prompt_payload->dump(), headers);
            if (!res.ok()) { std::cerr << "\nLLM Error: " << res.status().message() << std::endl; break; }

            if (!orchestrator.ProcessResponse(session_id, *res, group_id).ok()) break;

            auto hist = db.GetConversationHistory(session_id);
            if (!hist.ok() || hist->empty()) break;
            const auto& last = hist->back();

            if (last.status == "tool_call") {
                auto tc_or = orchestrator.ParseToolCall(last);
                if (!tc_or.ok()) { std::cerr << "\nTool Parse Error: " << tc_or.status().message() << std::endl; break; }
                auto& tc = *tc_or;
                
                // Human-readable tool call display
                std::string d_name = tc.name; std::string d_arg;
                if (tc.name == "execute_bash") { d_name = "Execute Bash"; d_arg = tc.args.value("command", ""); }
                else if (tc.name == "read_file") { d_name = "Read File"; d_arg = tc.args.value("path", ""); }
                else if (tc.name == "write_file") { d_name = "Write File"; d_arg = tc.args.value("path", ""); }
                else if (tc.name == "search_code") { d_name = "Search Code"; d_arg = tc.args.value("query", ""); }
                
                if (!d_arg.empty()) std::cout << "\r" << d_name << ": " << d_arg << std::endl;
                else std::cout << "\rCalling Tool: " << tc.name << std::endl;
                
                auto exec_res = tool_executor.Execute(tc.name, tc.args);
                std::string out = exec_res.ok() ? *exec_res : "Error: " + std::string(exec_res.status().message());
                
                nlohmann::json t_resp;
                if (provider == sentinel::Orchestrator::Provider::GEMINI) {
                    t_resp = {{"functionResponse", {{"name", tc.name}, {"response", {{"content", out}}}}}};
                } else {
                    t_resp = {{"role", "tool"}, {"tool_call_id", tc.id}, {"content", out}};
                }
                (void)db.AppendMessage(session_id, "tool", t_resp.dump(), last.tool_call_id, "completed", group_id);
                (void)db.Execute("UPDATE messages SET status = 'completed' WHERE id = " + std::to_string(last.id));
                needs_llm = true;
            } else { std::cout << "\rAssistant> " << last.content << std::endl; }
        }
    }
    return 0;
}
