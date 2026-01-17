#ifndef SLOP_GIT_HELPER_H_
#define SLOP_GIT_HELPER_H_

#include <string>
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace slop {

class GitHelper {
 public:
  static bool IsGitRepo();
  static absl::Status InitRepo();
  static absl::StatusOr<bool> HasChanges();
  static absl::Status CommitGroup(const std::string& group_id, const std::string& message);
  static absl::StatusOr<std::string> GetHashForGroupId(const std::string& group_id);
  static absl::Status UndoCommit(const std::string& group_id);
  static absl::StatusOr<std::string> GetDiff(const std::string& group_id);
};

}  // namespace slop

#endif  // SLOP_GIT_HELPER_H_
