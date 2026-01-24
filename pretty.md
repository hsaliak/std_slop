# TUI Aesthetic Overhaul: Test-Driven Execution Plan (COMPLETED)

This document outlines the roadmap for enhancing the `std::slop` terminal interface. Each phase follows a Red-Green-Refactor approach.

## Phase 1: Soft Palette & Modern Framing
**Goal:** Transition from 8-color ANSI to a refined 256-color theme with rounded borders.

- [x] **1.1 Research & Define Theme:** Select a palette (e.g., Nord or Catppuccin) and map it to TUI roles (Assistant, User, Tool, Dim).
- [x] **1.2 Extend `color.h`:** Add support for 256-color escape sequences and a `Theme` configuration struct.
- [x] **1.3 Unit Test `color.h`:** Add tests in `color_test.cpp` for RGB -> ANSI conversion.
- [x] **1.4 Implement Rounded Borders:** Update `PrintBorderedBlock` in `ui.cpp` to use `╭`, `╮`, `╯`, `╰`.

## Phase 2: Markdown-Lite Rendering
**Goal:** Render structural LLM output (bold, code) using ANSI styles.

- [x] **2.1 Prototype `RenderMarkdown`:** Create a string processor that handles `**bold**`, `*italic*`, and `` `inline code` ``.
- [x] **2.2 Unit Test Rendering:** Add cases to `ui_test.cpp` for nested formatting and ANSI-aware string length.
- [x] **2.3 Code Block Styling:** Implement background color changes for triple-backtick blocks.
- [x] **2.4 Integration:** Wire `RenderMarkdown` into `PrintAssistantMessage`.

## Phase 3: Semantic Iconography & Layout
**Goal:** Improve scannability with Unicode symbols and better whitespace.

- [x] **3.1 Unicode Icon Mapping:** Assign symbols (🤖, 👤, 🛠️, ⚠️) to message types.
- [x] **3.2 Unicode-Safe Width Logic:** Ensure `VisibleLength` handles multi-byte emoji without breaking box borders.
- [x] **3.3 Padding Refactor:** Add vertical spacing between conversation turns in `DisplayHistory`.

## Phase 4: Dynamic Status & Loading Feedback
**Goal:** Provide visual cues during long-running LLM or tool operations.

- [x] **4.1 Spinner Component:** Create a `Spinner` class that manages an async terminal animation.
- [x] **4.2 Signal Integration:** Hook into `Orchestrator` to trigger "Thinking..." state.
- [x] **4.3 Modeline Status:** Display current model/session info in a dedicated TUI line. (Implicitly handled by header improvements)

## Phase 5: Syntax Highlighting for Data
**Goal:** Colorize JSON and SQL results for rapid review.

- [x] **5.1 JSON Highlighter:** Regex-based highlighting for `PrintToolResultMessage`.
- [x] **5.2 Table Theme Update:** Apply the new palette to `PrintJsonAsTable` headers and separators.
