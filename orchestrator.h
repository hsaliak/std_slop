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

  void SetThrottle(int seconds);
  int GetThrottle() const { return throttle_; }

  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills = {});
  absl::Status ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id = "");
  
  absl::Status FinalizeInteraction(const std::string& session_id, const std::string& group_id);
  absl::Status UndoLastGroup(const std::string& session_id);

  // Rebuilds the session state (---STATE--- anchor) from the current window's history.
  absl::Status RebuildContext(const std::string& session_id);

  struct ToolCall {
    std::string name;
    std::string id; // For OpenAI
    nlohmann::json args;
  };
  absl::StatusOr<ToolCall> ParseToolCall(const Database::Message& msg);

  absl::StatusOr<std::vector<std::string>> GetModels(const std::string& api_key);
  absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token);

  int CountTokens(const nlohmann::json& prompt);

  std::vector<std::string> GetLastSelectedGroups() const { return last_selected_groups_; }

  // Exposed for rebuilding and testing
  absl::StatusOr<std::vector<Database::Message>> GetRelevantHistory(const std::string& session_id, int window_size);

 private:
  static constexpr int kMaxToolResultContext = 8192;

  // Helper methods for AssemblePrompt
  std::string BuildSystemInstructions(const std::string& session_id, const std::vector<std::string>& active_skills);
  nlohmann::json FormatGeminiPayload(const std::string& system_instruction, const std::vector<Database::Message>& history);
  nlohmann::json FormatOpenAIPayload(const std::string& system_instruction, const std::vector<Database::Message>& history);
  
  // Truncates content for context efficiency with metadata
  std::string SmarterTruncate(const std::string& content, size_t limit);

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
namespace slop {
  std::string GenerateGroupId();
}
