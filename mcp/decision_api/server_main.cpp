#include <algorithm>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "absl/status/status.h"

#include "core/http_client.h"
#include "mcp/decision_api/client.h"
#include "mcp/decision_api/config.h"
#include "mcp/decision_api/server.h"
#include "mcp/server/stdio.h"

#include <sys/stat.h>

namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr char kUsage[] = "usage: decision_api_server --config ABSOLUTE_PATH | --help\n";

absl::StatusOr<std::string> ReadConfigFile(const std::string& path) {
  struct stat info {};
  if (path.empty() || path.front() != '/' || lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return absl::InvalidArgumentError("config must be an absolute path to a regular file");
  }
  if ((info.st_mode & 0077) != 0) {
    return absl::PermissionDeniedError("config file permissions must restrict access to its owner");
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return absl::NotFoundError("cannot open decision API config file");
  std::string text;
  text.reserve(std::min<std::size_t>(static_cast<std::size_t>(info.st_size), kMaxConfigBytes));
  char buffer[4096];
  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    const std::size_t count = static_cast<std::size_t>(file.gcount());
    if (count > kMaxConfigBytes - text.size()) {
      return absl::ResourceExhaustedError("decision API config exceeds 64 KiB");
    }
    text.append(buffer, count);
  }
  if (file.bad()) return absl::UnavailableError("cannot read decision API config file");
  return text;
}

int Fail(const absl::Status& status) {
  std::cerr << "decision_api_server: " << status.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << kUsage;
    return 0;
  }
  if (argc != 3 || std::string(argv[1]) != "--config") {
    std::cerr << kUsage;
    return 2;
  }
  auto text = ReadConfigFile(argv[2]);
  if (!text.ok()) return Fail(text.status());
  auto config = slop::mcp::decision_api::ParseConfigText(*text);
  if (!config.ok()) return Fail(config.status());

  slop::HttpClient http_client(0, 0);
  auto client = std::make_shared<slop::mcp::decision_api::DecisionApiClient>(*config, &http_client);
  auto server = slop::mcp::decision_api::CreateServer(std::move(client), *config);
  if (!server.ok()) return Fail(server.status());

  std::signal(SIGPIPE, SIG_IGN);
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  const absl::Status status = slop::mcp::server::RunStdio(*server, std::cin, std::cout, std::cerr);
  return status.ok() ? 0 : Fail(status);
}
