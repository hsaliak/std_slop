
#include "acp/session_store.h"

#include <cctype>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/match.h"
#include "core/status_macros.h"
#include "core/json_utils.h"

namespace slop::acp {
namespace {

absl::StatusOr<std::string> NextGeneratedSessionId(Database* db) {
  ASSIGN_OR_RETURN(auto stmt,
                   db->Prepare("SELECT COALESCE(MAX(CAST(SUBSTR(id, 5) AS INTEGER)), 0) + 1 "
                               "FROM sessions WHERE id GLOB 'acp-[0-9]*'"));
  auto row_or = stmt->Step();
  if (!row_or.ok()) {
    return row_or.status();
  }
  if (!*row_or) {
    return std::string("acp-1");
  }
  const int64_t next_id_num = stmt->ColumnInt64(0);
  if (next_id_num <= 0) {
    return absl::InternalError("session_new_generated_id_invalid");
  }

  return absl::StrCat("acp-", next_id_num);
}

absl::StatusOr<bool> SessionExists(Database* db, std::string_view session_id) {
  ASSIGN_OR_RETURN(auto stmt, db->Prepare("SELECT 1 FROM sessions WHERE id = ? LIMIT 1"));
  RETURN_IF_ERROR(stmt->BindText(1, std::string(session_id)));
  return stmt->Step();
}

bool IsUniqueConstraintFailure(const absl::Status& status) {
  return absl::StrContains(status.message(), "UNIQUE constraint failed");
}

absl::Status InsertSession(Database* db, std::string_view session_id) {
  auto status = db->Execute("INSERT INTO sessions (id) VALUES (?)", std::string(session_id));
  if (!status.ok()) {
    return status;
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<SessionNewRequest> ParseSessionNewParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    return absl::InvalidArgumentError("session_new_params_must_be_object");
  }

  SessionNewRequest req;
  if (!json_at(params, "sessionId")) {
    return req;
  }
  auto session_id = json_get<std::string>(params, "sessionId");
  if (!session_id.has_value()) {
    return absl::InvalidArgumentError("session_new_session_id_must_be_string");
  }
  if (!IsValidSessionId(*session_id)) {
    return absl::InvalidArgumentError("session_new_session_id_invalid");
  }
  req.session_id = *session_id;
  return req;
}

bool IsValidSessionId(std::string_view session_id) {
  if (session_id.empty() || session_id.size() > 64) {
    return false;
  }
  for (char c : session_id) {
    const bool allowed = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<std::string> CreateSession(Database* db, const SessionNewRequest& request) {
  if (db == nullptr) {
    return absl::InvalidArgumentError("session_store_db_required");
  }

  std::string session_id;
  if (request.session_id.has_value()) {
    const std::string& requested_id = *request.session_id;
    ASSIGN_OR_RETURN(bool exists, SessionExists(db, requested_id));
    if (exists) {
      return absl::InvalidArgumentError("session_new_session_id_exists");
    }

    auto insert_status = InsertSession(db, requested_id);
    if (!insert_status.ok()) {
      if (IsUniqueConstraintFailure(insert_status)) {
        return absl::InvalidArgumentError("session_new_session_id_exists");
      }
      return insert_status;
    }
    return requested_id;
  }

  while (true) {
    ASSIGN_OR_RETURN(session_id, NextGeneratedSessionId(db));

    if (!IsValidSessionId(session_id)) {
      return absl::InvalidArgumentError("session_new_session_id_invalid");
    }

    auto insert_status = InsertSession(db, session_id);
    if (insert_status.ok()) {
      return session_id;
    }
    if (!IsUniqueConstraintFailure(insert_status)) {
      return insert_status;
    }
    // Another caller won the same generated ID; retry from current DB max.
    continue;
  }
}


}  // namespace slop::acp