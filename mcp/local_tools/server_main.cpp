#include <csignal>
#include <iostream>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "mcp/local_tools/server.h"
#include "mcp/local_tools/workspace_catalog.h"
#include "mcp/server/stdio.h"

namespace {

constexpr char kUsage[] = "usage: local_tools_server [--help]\n";

int Fail(const absl::Status& status) {
  std::cerr << "local_tools_server: " << status.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << kUsage;
    return 0;
  }
  if (argc != 1) {
    std::cerr << kUsage;
    return 2;
  }

  auto workspaces = slop::mcp::local_tools::WorkspaceCatalog::Create();
  if (!workspaces.ok()) return Fail(workspaces.status());
  auto server = slop::mcp::local_tools::CreateServer(std::move(*workspaces));
  if (!server.ok()) return Fail(server.status());

  std::signal(SIGPIPE, SIG_IGN);
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  const absl::Status status = slop::mcp::server::RunStdio(*server, std::cin, std::cout, std::cerr);
  return status.ok() ? 0 : Fail(status);
}
