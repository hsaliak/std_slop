# TDD Plan: `apply_patch` Tool

This document outlines the TDD-based approach to implementing the `apply_patch` tool in `std::slop`.

## Phase 1: Contract & Registry Verification
- **Step 1.1: Define Schema Test**. Add a test in `database_test.cpp` to verify `apply_patch` registration and schema validity.
- **Step 1.2: Implementation**. Update `Database::Init` to register the `apply_patch` tool.

## Phase 2: Tool Execution (Failing Tests)
- **Step 2.1: Simple Patch Test**. Add `ApplyPatch_Success` to `tool_executor_test.cpp` (matching a single block).
- **Step 2.2: Missing Match Test**. Add `ApplyPatch_FindNotFound` to verify `absl::StatusCode::kNotFound`.
- **Step 2.3: Ambiguity Test**. Add `ApplyPatch_AmbiguousMatch` to verify error on multiple matches.

## Phase 3: Core Implementation
- **Step 3.1: Tool Dispatch**. Update `ToolExecutor::Execute` to handle `apply_patch`.
- **Step 3.2: Patch Logic**. Implement `ToolExecutor::ApplyPatch` using `absl::StatusOr<std::string>` and RAII.

## Phase 4: Robustness & Edge Cases
- **Step 4.1: Multiple Patches Test**. Test atomic application of multiple patches in one call.
- **Step 4.2: Whitespace Sensitivity Test**. Verify handling of newlines and indentation in `find` blocks.

## Phase 5: LLM Integration
- **Step 5.1: System Prompt Update**. Document the tool in `system_prompt.md`.
- **Step 5.2: Manual Verification**. Verify LLM usage in a real session.
