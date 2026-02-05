#ifndef SLOP_SQL_UI_H_
#define SLOP_SQL_UI_H_

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "core/database.h"
#include "interface/color.h"
namespace slop {

void SetupTerminal();

void ShowBanner();
void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands = {});
std::string ReadLine(const std::string& modeline);
std::string OpenInEditor(const std::string& initial_content = "", const std::string& extension = ".txt");
absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit = 5);

// Formats the context JSON into a human-readable string.
std::string FormatAssembledContext(const std::string& json_str);

// Tries to display content in $EDITOR, falls back to stdout.
void SmartDisplay(const std::string& content);

// Convenience wrapper for FormatAssembledContext + SmartDisplay.
void DisplayAssembledContext(const std::string& json_str);

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
std::string WrapText(const std::string& text, size_t width = 0, const std::string& prefix = "",
                     const std::string& first_line_prefix = "");

// Flattens a JSON string into a human-readable key-value string.
// e.g. {"a": 1, "b": "c"} -> a: 1 | b: "c"
std::string FlattenJsonArgs(const std::string& json_str);

// Returns terminal width or 80 if detection fails.
size_t GetTerminalWidth();

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

std::string GetHelpText();
void ShowHelp();

}  // namespace slop

#endif  // SLOP_SQL_UI_H_
