#ifndef SLOP_SQL_ORCHESTRATOR_H_
#define SLOP_SQL_ORCHESTRATOR_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "core/database.h"
#include "core/http_client.h"
#include "core/orchestrator_strategy.h"

#include <nlohmann/json.hpp>

namespace slop {

class Orchestrator {
 public:
  struct TruncationSettings {
    // Maximum characters retained for a single tool result.
    size_t full_fidelity_limit = 5000;
  };
  struct Config {
    std::string model;
    std::string base_url;
    int throttle = 0;
    TruncationSettings truncation = {};
  };
  class Builder {
   public:
    Builder(Database* db, HttpClient* http_client);
    explicit Builder(const Orchestrator& orchestrator);
    Builder& WithModel(const std::string& model);
    Builder& WithBaseUrl(const std::string& url);
    Builder& WithThrottle(int seconds);
    Builder& WithDatabase(Database* db);
    absl::StatusOr<std::unique_ptr<Orchestrator>> Build();
    void BuildInto(Orchestrator* orchestrator);

   private:
    Database* db_;
    HttpClient* http_client_;
    Config config_;
    std::string active_agent_md_path_ = "./AGENTS.md";
  };
  Orchestrator(Database* db, HttpClient* http_client);
  std::string GetModel() const { return config_.model; }
  int GetThrottle() const { return config_.throttle; }
  std::string GetName() const { return strategy_ ? strategy_->GetName() : ""; }
  Builder Update() const { return Builder(*this); }
  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id,
                                                const std::vector<std::string>& active_skills = {});
  absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id, const std::string& system_instruction,
                                                 const std::vector<Database::Message>& history,
                                                 const std::vector<std::string>& active_skills = {});
  absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                      const std::string& group_id = "");
  std::optional<ResponseUsage> GetLastResponseUsage() const;
  absl::Status ForceAccordionReset(const std::string& session_id);
  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);
  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key, const std::string& account_id = "");
  absl::StatusOr<std::string> ExtractAssistantText(const std::string& response_body);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);
  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }
  absl::StatusOr<std::vector<Database::Message>> GetAccordionHistory(const std::string& session_id,
                                                                       bool force_reset = false);
  void UpdateStrategy();
  static std::string SmarterTruncate(const std::string& content, size_t limit, int message_id = -1);
  absl::Status LoadAgentMd(const std::string& path);
  void InjectAgentMd(std::string* system_instruction);
  std::string GetActiveAgentMdPath() const { return active_agent_md_path_; }
  Database* GetDatabase() const { return db_; }
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
  std::string BuildSystemInstructions(const std::string& session_id);
};
}  // namespace slop
#endif  // SLOP_SQL_ORCHESTRATOR_H_
