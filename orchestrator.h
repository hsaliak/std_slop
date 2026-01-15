#ifndef SENTINEL_SQL_ORCHESTRATOR_H_
#define SENTINEL_SQL_ORCHESTRATOR_H_

#include <string>
#include <vector>
#include "database.h"
#include "http_client.h"
#include <nlohmann/json.hpp>
#include "absl/status/statusor.h"

namespace sentinel {

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

  // Assembler: Builds the prompt for the current provider from the database state.
  absl::StatusOr<nlohmann::json> AssemblePrompt(const std::string& session_id, const std::vector<std::string>& active_skills = {});

  // Processes the LLM response, potentially updating the DB or identifying tool calls.
  absl::Status ProcessResponse(const std::string& session_id, const std::string& response_json, const std::string& group_id = "");

  struct ToolCall {
    std::string name;
    std::string id;
    nlohmann::json args;
  };
  absl::StatusOr<ToolCall> ParseToolCall(const Database::Message& msg);

 private:
  Database* db_;
  HttpClient* http_client_;
  Provider provider_ = Provider::GEMINI;
  std::string model_;
};

}  // namespace sentinel

#endif  // SENTINEL_SQL_ORCHESTRATOR_H_
