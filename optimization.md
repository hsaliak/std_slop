# Optimization Plan: Reducing Repeated Heap Allocations

This document outlines the TDD-based approach to optimizing heap allocations in `std::slop`.

## Phase 1: HttpClient - Resource Reuse
**Goal**: Reduce redundant allocations of CURL handles and header lists during retries.
- **TDD**: Add `RetryFunctionalTest` to `http_client_test.cpp`.
- **Optimization**: Move `curl_easy_init` and `curl_slist_append` outside the retry loop in `HttpClient::ExecuteWithRetry`.
- **Regression Prevention**: Use RAII (e.g., `unique_ptr` with custom deleter) to ensure `curl_slist_free_all` is called exactly once.

## Phase 2: Database - SQL-Level Filtering
**Goal**: Offload message filtering to SQLite to avoid fetching thousands of strings into memory.
- **TDD**: Add `GetHistoryWindowed` to `database_test.cpp`.
- **Optimization**: Update `Database::GetConversationHistory` to use a windowed subquery: `WHERE group_id IN (SELECT DISTINCT group_id ... LIMIT ?)`.
- **Regression Prevention**: Ensure sorting maintains chronological order for the LLM while correctly selecting the latest groups.

## Phase 3: Orchestrator - Allocation Hygiene
**Goal**: Minimize copies and small allocations in the prompt assembly pipeline.
- **TDD**: Verify against existing `OrchestratorTest`.
- **Optimization**: 
    - Use `reserve()` on result vectors.
    - Use `std::move` when transferring `Message` objects.
- **Regression Prevention**: Verify bit-for-bit parity of `AssemblePrompt` JSON output before and after changes.

## Final Verification
1. Run full suite: `bazel test ...`.
2. Manual smoke test of turn-based interaction.
3. (Optional) Check for leaks with ASAN.

## Remaining Opportunities (Post-Verification)
These items were identified during the final code review as minor deviations or further improvements:
1. **HttpClient**: Move `response_string` outside the `while(true)` loop in `ExecuteWithRetry` and use `.clear()` to reuse the allocation across retries.
2. **Database**: Move `parsing_strategy` and tool-related filtering from `Orchestrator::GetRelevantHistory` into the `Database::GetConversationHistory` SQL query to further reduce string fetching.
3. **Orchestrator Strategies**: 
    - Eliminate the redundant JSON array copy in `GeminiOrchestrator::AssemblePayload` (where `contents` is filtered into `valid_contents`).
    - Replace string concatenation (e.g. `"..." + content`) with `absl::StrCat` or `absl::StrAppend` in strategy payload assembly.
