#ifndef SLOP_SQL_ORCHESTRATOR_H_
#define SLOP_SQL_ORCHESTRATOR_H_

#include <string>
#include <vector>
#include "database.h"
#include "http_client.h"
#include <nlohmann/json.hpp>
#include "absl/status/statusor.h"

namespace slop {

class Orchestrator {
 public:
  enum class Provider {
    GEMINI,
    OPENAI
  };

  Orchestrator(Database* db, HttpClient* http_client);

  void SetProvider(Provider provider) { provider_ = provider; }
  Provider GetProvider() const { return provider_; }
  void SetModel(const std::string& model) { model_ = model; }
  std::string GetModel() const { return model_; }

  void SetGcaMode(bool enabled) { gca_mode_ = enabled; }
  void SetProjectId(const std::string& project_id) { project_id_ = project_id; }

  void SetThrottle(int seconds) { throttle_ = seconds; }
  int GetThrottle() const { return throttle_; }

  // Assembler: Builds the prompt for the current provider from the database state.
  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills = {});
  int CountTokens(const nlohmann::json& prompt);

  // Fetches available models from the provider.
  absl::StatusOr<std::vector<std::string>> GetModels(const std::string& api_key = "");

  // Fetches Gemini user quota information.
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

  // Processes the LLM response, potentially updating the DB or identifying tool calls.
  absl::Status ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id = "");

  struct ToolCall {
    std::string name;
    std::string id;
    nlohmann::json args;
  };
  absl::StatusOr<ToolCall> ParseToolCall(const Database::Message& msg);

  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }

 private:
  Database* db_;
  HttpClient* http_client_;
  Provider provider_ = Provider::GEMINI;
  std::string model_;
  bool gca_mode_ = false;
  std::string project_id_;
  int throttle_ = 0;
  std::vector<std::string> last_selected_groups_;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_H_
