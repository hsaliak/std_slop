#ifndef SLOP_RPC_SLOP_SERVICE_H_
#define SLOP_RPC_SLOP_SERVICE_H_

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "app/runtime_bootstrap.h"
#include "core/database.h"
#include "core/http_client.h"
#include "rpc/server_config.h"
#include "rpc/slop_rpc.grpc.pb.h"

namespace slop::rpc::v1 {

class SlopServiceImpl final : public SlopService::Service {
 public:
  SlopServiceImpl(ServerRuntimeConfig server_config, slop::RuntimeBootstrap runtime, slop::Database* db);

  grpc::Status RunPrompt(grpc::ServerContext* context, const RunPromptRequest* request,
                         RunPromptResponse* response) override;

 private:
  ServerRuntimeConfig server_config_;
  slop::RuntimeBootstrap runtime_;
  slop::Database* db_;  // Not owned. Must outlive this service.
  std::string configured_model_;
};

absl::Status RunSlopRpcService(const ServerRuntimeConfig& server_config);

void ConfigureRpcOpenAiOAuthHandler(slop::HttpClient* http_client,
                                    std::shared_ptr<slop::OAuthHandler>* handler);

}  // namespace slop::rpc::v1

#endif  // SLOP_RPC_SLOP_SERVICE_H_
