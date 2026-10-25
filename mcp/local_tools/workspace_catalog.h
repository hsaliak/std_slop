#ifndef SLOP_MCP_LOCAL_TOOLS_WORKSPACE_CATALOG_H_
#define SLOP_MCP_LOCAL_TOOLS_WORKSPACE_CATALOG_H_

#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace slop::mcp::local_tools {

struct Workspace {
  std::string id;
  std::filesystem::path root;
  bool is_default = false;
};

class WorkspaceCatalog {
 public:
  static absl::StatusOr<WorkspaceCatalog> Create();
  static absl::StatusOr<WorkspaceCatalog> Create(const std::filesystem::path& launch_directory);

  absl::StatusOr<std::vector<Workspace>> List() const;
  absl::StatusOr<Workspace> Resolve(absl::string_view workspace_id) const;

 private:
  WorkspaceCatalog(std::filesystem::path launch_directory, std::filesystem::path git_top_level,
                   std::filesystem::path git_common_directory, bool is_git_repository);

  std::filesystem::path launch_directory_;
  std::filesystem::path git_top_level_;
  std::filesystem::path git_common_directory_;
  bool is_git_repository_;
};

}  // namespace slop::mcp::local_tools

#endif  // SLOP_MCP_LOCAL_TOOLS_WORKSPACE_CATALOG_H_
