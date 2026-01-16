#ifndef SLOP_SQL_UI_H_
#define SLOP_SQL_UI_H_

#include "absl/status/status.h"
#include <string>
#include <vector>
#include "database.h"

namespace slop {

void SetupTerminal();
std::string ReadLine(const std::string& prompt, const std::string& session_id);
std::string OpenInEditor(const std::string& initial_content = "");
absl::Status DisplayHistory(slop::Database& db, const std::string& session_id, int limit = 5, const std::vector<std::string>& selected_groups = {});
absl::Status PrintJsonAsTable(const std::string& json_str);
void DisplayAssembledContext(const std::string& json_str);

} // namespace slop

#endif // SLOP_SQL_UI_H_
