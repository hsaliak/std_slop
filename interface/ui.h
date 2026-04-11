#ifndef SLOP_INTERFACE_UI_H_
#define SLOP_INTERFACE_UI_H_

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "core/database.h"
#include "interface/animator.h"
#include "interface/color.h"
#include "interface/renderer.h"

namespace slop {

void ShowBanner();

std::string OpenInEditor(const std::string& initial_content = "", const std::string& extension = ".txt");
absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit = 3);

// Formats the context JSON into a human-readable string.
std::string FormatAssembledContext(const std::string& json_str);

// Tries to display content in $EDITOR, falls back to stdout.
void SmartDisplay(const std::string& content, bool is_markdown = false);
void DisplayAssembledContext(const std::string& json_str);

// Convenience wrapper for FormatAssembledContext + SmartDisplay.

/**
 * @brief Renders markdown content to the terminal.
 */
void PrintMarkdown(const std::string& markdown, const std::string& prefix = "");

/**
 * @brief Logs an error status if it is not OK.
 *
 * @param status The status to handle.
 * @param context Optional context message to prepend to the error.
 */
void HandleStatus(const absl::Status& status, const std::string& context = "");

// Wraps text to a specific width, preserving newlines and being ANSI-aware.
// Optionally prepends a prefix to each line.

// Flattens a JSON string into a human-readable key-value string.
// e.g. {"a": 1, "b": "c"} -> a: 1 | b: "c"
std::string FlattenJsonArgs(const std::string& json_str);

// Returns terminal width or 80 if detection fails.

/**
 * @brief High-level methods for centralized UI message formatting.
 */
void PrintAssistantMessage(const std::string& content, const std::string& prefix = "", int tokens = 0);
void PrintToolCallMessage(const std::string& name, const std::string& args, const std::string& prefix = "",
                          int tokens = 0);
void PrintToolResultMessage(const std::string& name, const std::string& result, const std::string& status = "completed",
                            const std::string& prefix = "");

/**
 * @brief Unified message printer that handles all roles and formatting.
 *
 * @param msg The message to print.
 * @param prefix Optional prefix for indentation.
 */
void PrintMessage(const Database::Message& msg, const std::string& prefix = "");

std::string GetHelpText(bool include_core_operations = true);
std::string GetCliHelpText();
void ShowHelp();

/**
 * @brief Render markdown text to ANSI-encoded string.
 *
 * @param markdown The markdown text to render.
 * @param prefix Optional prefix for each line.
 * @param rendered Output string that will contain the ANSI-rendered text.
 */
void RenderMarkdown(const std::string& markdown, const std::string& prefix, std::string* rendered);

}  // namespace slop
#endif  // SLOP_INTERFACE_UI_H_
