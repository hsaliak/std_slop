#ifndef SLOP_SQL_ORCHESTRATOR_H_
#define SLOP_SQL_ORCHESTRATOR_H_

#include "database.h"
#include "http_client.h"
#include "orchestrator_strategy.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include <nlohmann/json.hpp>

namespace slop {

class Orchestrator {
 public:
  enum class Provider { GEMINI, OPENAI };

  struct Config {
    Provider provider = Provider::GEMINI;
    std::string model;
    bool gca_mode = false;
    std::string project_id;
    std::string base_url;
    int throttle = 0;
    bool strip_reasoning = false;
  };

  class Builder {
   public:
    Builder(Database* db, HttpClient* http_client);
    explicit Builder(const Orchestrator& orchestrator);

    Builder& WithProvider(Provider provider);
    Builder& WithModel(const std::string& model);
    Builder& WithGcaMode(bool enabled);
    Builder& WithProjectId(const std::string& project_id);
    Builder& WithBaseUrl(const std::string& url);
    Builder& WithThrottle(int seconds);
    Builder& WithStripReasoning(bool enabled);

    std::unique_ptr<Orchestrator> Build();
    void BuildInto(Orchestrator* orchestrator);

   private:
    Database* db_;
    HttpClient* http_client_;
    Config config_;
  };

  // Constructor is public to allow stack allocation if desired,
  // but Builder is preferred for complex configuration.
  Orchestrator(Database* db, HttpClient* http_client);

  Provider GetProvider() const { return provider_; }
  std::string GetModel() const { return model_; }
  int GetThrottle() const { return throttle_; }

  Builder Update() const { return Builder(*this); }

  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id,
                                                const std::vector<std::string>& active_skills = {});
  absl::Status ProcessResponse(const std::string& session_id, const std::string& response_json,
                               const std::string& group_id = "");

  // Rebuilds the session state (---STATE--- anchor) from the current window's history.
  absl::Status RebuildContext(const std::string& session_id);

  absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg);

  absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

  int CountTokens(const nlohmann::json& prompt);

  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }

  // Exposed for rebuilding and testing
  absl::StatusOr<std::vector<Database::Message>> GetRelevantHistory(const std::string& session_id, int window_size);

  // Refactored: UpdateStrategy is now called by Build() or BuildInto()
  void UpdateStrategy();

 private:
  friend class Builder;

  Database* db_;
  HttpClient* http_client_;
  Provider provider_ = Provider::GEMINI;
  std::string model_;
  bool gca_mode_ = false;
  std::string project_id_;
  std::string base_url_;
  int throttle_ = 0;
  bool strip_reasoning_ = false;
  std::vector<std::string> last_selected_groups_;

  std::unique_ptr<OrchestratorStrategy> strategy_;

  // Helper methods for AssemblePrompt
  std::string BuildSystemInstructions(const std::string& session_id, const std::vector<std::string>& active_skills);
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_H_
