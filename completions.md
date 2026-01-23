# Autocompletion for / commands

This document outlines the TDD-based approach to implementing command autocompletion in `std::slop`.

## Phase 1: Data Access & Logic (TDD)
**Goal:** Provide a way to retrieve registered commands and filter them by prefix.

- **Step 1.1: Update `CommandHandler`**. Add a public method `std::vector<std::string> GetCommandNames() const`.
- **Step 1.2: Verification**. Write a unit test in `command_handler_test.cpp` ensuring this returns all registered keys (e.g., `/session`, `/undo`).
- **Step 1.3: Create `CommandCompleter` Utility**. Implement a stateless utility function `std::vector<std::string> FilterCommands(const std::string& prefix, const std::vector<std::string>& commands)`.
- **Step 1.4: TDD (New Test File `completer_test.cpp`)**. Add tests for prefix matching, empty prefixes, and non-matching prefixes.

## Phase 2: Readline Bridge
**Goal:** Bridge the C++ class instance to the C-style `readline` callback.

- **Step 2.1: Static Bridge in `UI`**. Define `CommandCompletionProvider` and `CommandGenerator` as static methods or in a dedicated bridge namespace.
- **Step 2.2: State Management**. Implement a mechanism to register the active `CommandHandler` with the bridge.

## Phase 3: Integration in `ui.cpp`
**Goal:** Activate autocompletion during the prompt loop.

- **Step 3.1: Hook Registration**. In `SetupTerminal()` or `PromptUser()`, set `rl_attempted_completion_function` to the bridge provider.
- **Step 3.2: Generator Implementation**. Implement `CommandGenerator` to iterate through matches from `CommandHandler::GetCommandNames()`.

## Phase 4: Verification & Refinement
**Goal:** Ensure the feature works as expected in a real session.

- **Step 4.1: Manual Test**. Verify completion with single and double `Tab` presses, and with different prefixes.
- **Step 4.2: Context Sensitivity (Optional)**. Explore completing session IDs or tool names.
