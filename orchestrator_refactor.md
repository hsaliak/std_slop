# Orchestrator Strategy Pattern Refactor Plan

## Overview
The current `Orchestrator` implementation uses centralized `if/else` logic to handle different LLM providers (Gemini, OpenAI). This refactor moves provider-specific logic into a **Strategy Pattern**, improving extensibility and maintainability.

## 1. Define the Strategy Interface
Create a new header `orchestrator_strategy.h` defining the `OrchestratorStrategy` abstract base class.

```cpp
class OrchestratorStrategy {
 public:
  virtual ~OrchestratorStrategy() = default;

  virtual absl::StatusOr<nlohmann::json> AssemblePayload(
      const std::string& session_id,
      const std::string& system_instruction,
      const std::vector<Database::Message>& history) = 0;

  virtual absl::Status ProcessResponse(
      const std::string& session_id,
      const std::string& response_json,
      const std::string& group_id) = 0;

  virtual absl::StatusOr<std::vector<Orchestrator::ToolCall>> ParseToolCalls(
      const Database::Message& msg) = 0;

  virtual absl::StatusOr<std::vector<Orchestrator::ModelInfo>> GetModels(const std::string& api_key) = 0;
  virtual absl::StatusOr<nlohmann::json> GetQuota(const std::string& oauth_token) = 0;
  virtual int CountTokens(const nlohmann::json& prompt) = 0;
};
```

## 2. Implement Concrete Strategies
Implement specialized classes for each provider:
*   **`OpenAiOrchestrator`**: Handles OpenAI payload formatting and tool call parsing.
*   **`GeminiOrchestrator`**: Handles Gemini's specific role requirements and function declarations.
*   **`GeminiGcaOrchestrator`**: Inherits from `GeminiOrchestrator`, adding GCA wrapping logic.

## 3. Refactor the `Orchestrator` (The Context)
*   Remove `provider_` and `gca_mode_` logic branches.
*   Add `std::unique_ptr<OrchestratorStrategy> strategy_`.
*   Implement `UpdateStrategy()` to dynamically swap implementations when settings change.
*   Delegate provider-specific calls to `strategy_`.
*   Retain provider-agnostic logic (e.g., `BuildSystemInstructions`, `GetRelevantHistory`).

## 4. File Organization
*   `orchestrator.h / .cpp`: Main manager and public API.
*   `orchestrator_strategy.h`: Strategy interface and common structs.
*   `orchestrator_gemini.h / .cpp`: Gemini-specific implementations.
*   `orchestrator_openai.h / .cpp`: OpenAI-specific implementation.

## 5. Migration Steps
1.  **Extract Structs**: Move `ToolCall` and `ModelInfo` to `orchestrator_strategy.h`.
2.  **Define Interface**: Implement the `OrchestratorStrategy` base class.
3.  **Port Gemini Logic**: Move code from `FormatGeminiPayload` and Gemini `ProcessResponse` blocks.
4.  **Port OpenAI Logic**: Move code from `FormatOpenAIPayload` and OpenAI `ProcessResponse` blocks.
5.  **Inject Strategy**: Update `Orchestrator` to use the strategy pointer.
6.  **Verify**: Ensure all tests in `orchestrator_test.cpp` pass.
