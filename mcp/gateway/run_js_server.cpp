#include <unistd.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "core/http_client.h"
#include "mcp/client/client.h"
#include "mcp/client/stdio_transport.h"
#include "mcp/gateway/broker.h"
#include "mcp/gateway/catalog.h"
#include "mcp/gateway/config.h"
#include "mcp/gateway/server.h"
#include "mcp/gateway/trace_logger.h"
#include "mcp/gateway/worker.h"
#include "mcp/server/stdio.h"

namespace {

constexpr char kUsage[] = "usage: run_js_server --config PATH | --help\n";

absl::StatusOr<std::string> ReadConfigFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return absl::NotFoundError("cannot open configuration file");
  constexpr std::size_t kMaxConfigBytes = 1024 * 1024;
  std::string contents;
  char buffer[4096];
  while (file) {
    file.read(buffer, sizeof(buffer));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      contents.append(buffer, static_cast<std::size_t>(count));
      if (contents.size() > kMaxConfigBytes)
        return absl::ResourceExhaustedError("configuration file exceeds maximum size");
    }
  }
  if (file.bad()) return absl::UnavailableError("failed to read configuration file");
  return contents;
}

absl::StatusOr<std::string> ResolveExecutablePath(const std::string& argv0) {
  std::vector<std::filesystem::path> candidates;
  if (argv0.find('/') != std::string::npos) {
    candidates.emplace_back(argv0);
  } else {
    const char* path_value = std::getenv("PATH");
    if (path_value == nullptr) return absl::NotFoundError("PATH is not set");
    std::stringstream path(path_value);
    std::string directory;
    while (std::getline(path, directory, ':')) {
      candidates.emplace_back(std::filesystem::path(directory) / argv0);
    }
  }
  for (const auto& candidate : candidates) {
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(candidate, error);
    if (!error && std::filesystem::is_regular_file(resolved, error) && access(resolved.c_str(), X_OK) == 0) {
      return resolved.string();
    }
  }
  return absl::NotFoundError("cannot resolve run_js_server executable path");
}

int Fail(const absl::Status& status) {
  std::cerr << "run_js_server: " << status.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--worker-fd") {
    int fd = -1;
    const std::string descriptor(argv[2]);
    const auto parsed = std::from_chars(descriptor.data(), descriptor.data() + descriptor.size(), fd);
    if (parsed.ec != std::errc() || parsed.ptr != descriptor.data() + descriptor.size() || fd < 3) return 2;
    return slop::mcp::gateway::RunWorkerMode(fd);
  }
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
  auto config = slop::mcp::gateway::ParseConfigText(*text);
  if (!config.ok()) return Fail(config.status());

  std::shared_ptr<slop::mcp::gateway::TraceLogger> trace_logger;
  if (config->trace_log_path.has_value()) {
    auto logger = slop::mcp::gateway::TraceLogger::Open(*config->trace_log_path);
    if (!logger.ok()) return Fail(logger.status());
    trace_logger = std::move(*logger);
  }

  std::vector<std::unique_ptr<slop::HttpClient>> http_clients;
  http_clients.reserve(config->servers.size());
  std::vector<slop::mcp::gateway::DownstreamClient> downstreams;
  downstreams.reserve(config->servers.size());
  slop::mcp::ClientOptions client_options;
  client_options.client_info = {"slop-run-js-gateway", "1.0", std::nullopt};
  client_options.initialization_timeout = absl::Seconds(10);
  client_options.request_timeout = absl::Seconds(30);
  for (const auto& server : config->servers) {
    absl::StatusOr<std::unique_ptr<slop::mcp::Client>> client =
        absl::InvalidArgumentError("unsupported downstream transport");
    if (server.transport == slop::mcp::gateway::ServerTransport::kStdio) {
      slop::mcp::StdioTransportOptions transport;
      transport.command = server.command;
      transport.args = server.args;
      client = slop::mcp::ConnectStdioMcp(std::move(transport), client_options);
    } else {
      auto http_client = std::make_unique<slop::HttpClient>();
      slop::mcp::StreamableHttpConfig transport;
      transport.endpoint_url = server.endpoint_url;
      transport.request_timeout = client_options.request_timeout;
      client = slop::mcp::ConnectMcp(transport, client_options, http_client.get());
      if (client.ok()) http_clients.push_back(std::move(http_client));
    }
    if (!client.ok()) return Fail(client.status());
    downstreams.push_back({server.alias, server.allow_tools, std::move(*client)});
  }
  auto catalog = slop::mcp::gateway::Catalog::Create(std::move(downstreams));
  if (!catalog.ok()) return Fail(catalog.status());
  auto broker = slop::mcp::gateway::ParallelBroker::Create(std::move(*catalog), trace_logger);
  if (!broker.ok()) return Fail(broker.status());
  auto executable_path = ResolveExecutablePath(argv[0]);
  if (!executable_path.ok()) return Fail(executable_path.status());
  slop::mcp::gateway::RunJsExecutor executor = [path = *executable_path](
                                                   const std::string& code, const nlohmann::json& input,
                                                   slop::js_runtime::AsyncToolBroker* tool_broker,
                                                   slop::js_runtime::RuntimeOptions options, std::uint64_t trace_id) {
    return slop::mcp::gateway::ExecuteInWorker(path, code, input, tool_broker, options, trace_id);
  };
  slop::js_runtime::RuntimeOptions runtime_options;
  runtime_options.timeout = std::chrono::milliseconds(config->run_timeout_ms);
  auto server = slop::mcp::gateway::CreateServer(*broker, runtime_options, std::move(executor), trace_logger);
  if (!server.ok()) return Fail(server.status());

  std::signal(SIGPIPE, SIG_IGN);
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  const absl::Status status = slop::mcp::server::RunStdio(*server, std::cin, std::cout, std::cerr);
  return status.ok() ? 0 : Fail(status);
}
