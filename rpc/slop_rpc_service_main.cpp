#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "rpc/server_config.h"
#include "rpc/slop_service.h"

ABSL_FLAG(std::string, server_config, "docs/impl/rpc/server.cfg", "Path to slop RPC server textproto config.");

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  auto config_or = slop::rpc::v1::LoadServerRuntimeConfig(absl::GetFlag(FLAGS_server_config));
  if (!config_or.ok()) {
    std::cerr << "Failed to load server config: " << config_or.status() << std::endl;
    return 1;
  }

  absl::Status status = slop::rpc::v1::RunSlopRpcService(*config_or);
  if (!status.ok()) {
    std::cerr << "slop_rpc_service failed: " << status << std::endl;
    return 1;
  }
  return 0;
}
