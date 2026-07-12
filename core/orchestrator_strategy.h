#ifndef SLOP_SQL_ORCHESTRATOR_STRATEGY_H_
#define SLOP_SQL_ORCHESTRATOR_STRATEGY_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

#include "core/database.h"

#include <nlohmann/json.hpp>

namespace slop {

class Orchestrator;  // Forward declaration

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json args;
};

struct ModelInfo {
  std::string id;
  std::string name;
};

struct ResponseUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  std::optional<int> cached_input_tokens;
};

class OrchestratorStrategy {
 public:
  virtual ~OrchestratorStrategy() = default;

  virtual std::string GetName() const = 0;

  // Assembles the JSON payload for the specific provider.
  // The system_instruction and history are provided by the Orchestrator.
  virtual absl::StatusOr<nlohmann::json> AssemblePayload(const std::string& session_id,
                                                         const std::string& system_instruction,
                                                         const std::vector<Database::Message>& history,
                                                         const std::vector<std::string>& active_skills) = 0;

  // Parses the provider's response, records usage, and appends messages to the DB.
  // Returns the total tokens used in this turn.
  virtual absl::StatusOr<int> ProcessResponse(const std::string& session_id, const std::string& response_json,
                                              const std::string& group_id) = 0;

  virtual std::optional<ResponseUsage> GetLastResponseUsage() const = 0;

  // Extracts ToolCalls from a database message.
  virtual absl::StatusOr<std::vector<ToolCall>> ParseToolCalls(const Database::Message& msg) = 0;

  // Extracts assistant text from a one-shot provider response without writing
  // conversation state or tool calls to the database.
  virtual absl::StatusOr<std::string> ExtractAssistantText(const std::string& response_body) = 0;

  // Provider-specific API interactions.
  virtual absl::StatusOr<std::vector<ModelInfo>> GetModels(const std::string& api_key,
                                                           const std::string& account_id) = 0;
  virtual absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) = 0;
};

}  // namespace slop

#endif  // SLOP_SQL_ORCHESTRATOR_STRATEGY_H_
