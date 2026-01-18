#ifndef SLOP_SQL_UI_H_
#define SLOP_SQL_UI_H_

#include "absl/status/status.h"
#include <string>
#include <vector>
#include "database.h"

namespace slop {

void SetupTerminal();
void ShowBanner();
std::string ReadLine(const std::string& prompt, const std::string& session_id);
std::string OpenInEditor(const std::string& initial_content = "");
absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit = 5, const std::vector<std::string>& selected_groups = {});
absl::Status PrintJsonAsTable(const std::string& json_str);

// Formats the context JSON into a human-readable string.
std::string FormatAssembledContext(const std::string& json_str);

// Tries to display content in $EDITOR, falls back to stdout.
void SmartDisplay(const std::string& content);

// Convenience wrapper for FormatAssembledContext + SmartDisplay.
void DisplayAssembledContext(const std::string& json_str);

// Wraps text to a specific width, preserving newlines and being ANSI-aware.
std::string WrapText(const std::string& text, size_t width = 80);

} // namespace slop

#endif // SLOP_SQL_UI_H_
