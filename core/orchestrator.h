#ifndef SLOP_SQL_ORCHESTRATOR_H_
#define SLOP_SQL_ORCHESTRATOR_H_
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include "core/database.h"
#include "core/http_client.h"
#include "core/orchestrator_strategy.h"

#include <nlohmann/json.hpp>
namespace slop {
class Orchestrator {
 public:
  enum class Provider { GEMINI, OPENAI };
  enum class OpenAiApiStyle { CHAT_COMPLETIONS, RESPONSES };
  struct TruncationSettings {
    // truncation character length for full fidelity
    size_t active_full_fidelity_limit = 5000;
    // truncated character length for degraded
    size_t active_degraded_limit = 2000;  // its very aggressive
    // truncated character length for inactive
    size_t inactive_limit = 1000;  // super aggressive
    // how many messages should be considered "full fidelity"
    size_t full_fidelity_count = 10;
  };
  struct Config {
    Provider provider = Provider::GEMINI;
    std::string model;
    std::string base_url;
    int throttle = 0;
    OpenAiApiStyle openai_api_style = OpenAiApiStyle::CHAT_COMPLETIONS;
    TruncationSettings truncation = {};
  };
  class Builder {
   public:
    Builder(Database* db, HttpClient* http_client);
    explicit Builder(const Orchestrator& orchestrator);
    Builder& WithProvider(Provider provider);
    Builder& WithModel(const std::string& model);
    Builder& WithBaseUrl(const std::string& url);
    Builder& WithThrottle(int seconds);
    Builder& WithOpenAiApiStyle(OpenAiApiStyle style);
    Builder& WithDatabase(Database* db);
    absl::StatusOr<std::unique_ptr<Orchestrator>> Build();
    void BuildInto(Orchestrator* orchestrator);

   private:
    Database* db_;
    HttpClient* http_client_;
    Config config_;
    std::string active_agent_md_path_ = "./AGENTS.md";
  };
  // Constructor is public to allow stack allocation if desired,
  // but Builder is preferred for complex configuration.
  Orchestrator(Database* db, HttpClient* http_client);
  Provider GetProvider() const { return config_.provider; }
  std::string GetModel() const { return config_.model; }
  OpenAiApiStyle GetOpenAiApiStyle() const { return config_.openai_api_style; }
  int GetThrottle() const { return config_.throttle; }
  std::string GetName() const { return strategy_ ? strategy_->GetName() : ""; }
  Builder Update() const { return Builder(*this); }
  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id,
                                                const std::vector<std::string>& active_skills = {});
  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id = "");
  // Rebuilds the session state (### STATE anchor) from the current window's history.
  absl::Status RebuildContext(const std::string& session_id);
  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);
  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key, const std::string& account_id = "");
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);
  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }
  // Exposed for rebuilding and testing
  absl::StatusOr<std::vector<Database::Message>> GetRelevantHistory(const std::string& session_id, int window_size);
  // Refactored: UpdateStrategy is now called by Build() or BuildInto()
  void UpdateStrategy();
  // Utility for truncating large tool results.
  static std::string SmarterTruncate(const std::string& content, size_t limit, int message_id = -1);
  // Extracts the ### STATE block from a message, terminating at the next header or EOF.
  static std::optional<std::string> ExtractState(const std::string& text);
  absl::Status LoadAgentMd(const std::string& path);
  void InjectAgentMd(std::string* system_instruction);
  std::string GetActiveAgentMdPath() const { return active_agent_md_path_; }
  Database* GetDatabase() const { return db_; }  // Skills Management
  absl::Status ReloadAllSkills();
  absl::Status ReloadSkills(const std::string& directory = "./skills");
  absl::StatusOr<std::string> ListSkills() const;
  void InjectSkillsSummary(std::string* system_instruction);

 private:
  friend class Builder;
  Database* db_;
  HttpClient* http_client_;
  Config config_;
  std::string active_agent_md_path_ = "./AGENTS.md";
  std::vector<std::string> last_selected_groups_;
  std::unique_ptr<OrchestratorStrategy> strategy_;
  // Helper methods for AssemblePrompt
  std::string BuildSystemInstructions(const std::string& session_id, const std::vector<std::string>& active_skills);
};
}  // namespace slop
#endif  // SLOP_SQL_ORCHESTRATOR_H_
