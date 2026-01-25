#ifndef SLOP_SQL_UI_H_
#define SLOP_SQL_UI_H_

#include "database.h"

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "color.h"
namespace slop {

void SetupTerminal();

void ShowBanner();
void SetCompletionCommands(const std::vector<std::string>& commands,
                           const absl::flat_hash_map<std::string, std::vector<std::string>>& sub_commands = {});
std::string ReadLine(const std::string& modeline);
std::string OpenInEditor(const std::string& initial_content = "");
absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit = 5);
absl::Status PrintJsonAsTable(const std::string& json_str);

// Formats the context JSON into a human-readable string.
std::string FormatAssembledContext(const std::string& json_str);

// Tries to display content in $EDITOR, falls back to stdout.
void SmartDisplay(const std::string& content);

// Convenience wrapper for FormatAssembledContext + SmartDisplay.
void DisplayAssembledContext(const std::string& json_str);

/**
 * @brief Logs an error status if it is not OK.
 *
 * @param status The status to handle.
 * @param context Optional context message to prepend to the error.
 */
void HandleStatus(const absl::Status& status, const std::string& context = "");

// Wraps text to a specific width, preserving newlines and being ANSI-aware.
std::string WrapText(const std::string& text, size_t width = 0);

// Returns terminal width or 80 if detection fails.
size_t GetTerminalWidth();



/**
 * @brief High-level methods for centralized UI message formatting.
 */
void PrintAssistantMessage(const std::string& content, const std::string& skill_info = "");
void PrintToolCallMessage(const std::string& name, const std::string& args);
void PrintToolResultMessage(const std::string& result);

}  // namespace slop

#endif  // SLOP_SQL_UI_H_
