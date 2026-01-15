#ifndef SENTINEL_SQL_UI_H_
#define SENTINEL_SQL_UI_H_

#include "absl/status/status.h"

#include <string>
#include "database.h"

namespace sentinel {

std::string OpenInEditor(const std::string& initial_content = "");
absl::Status DisplayHistory(sentinel::Database& db, const std::string& session_id, int limit = 5);
absl::Status PrintJsonAsTable(const std::string& json_str);

} // namespace sentinel

#endif // SENTINEL_SQL_UI_H_
