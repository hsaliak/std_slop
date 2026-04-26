#ifndef SLOP_RPC_REQUEST_VALIDATION_H_
#define SLOP_RPC_REQUEST_VALIDATION_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "rpc/server_config.h"
#include "rpc/slop_rpc.pb.h"

namespace slop::rpc::v1 {

struct ValidatedRunPromptRequest {
  std::string prompt;
  std::string session_id;
  std::vector<std::string> active_skills;
  std::optional<std::string> model_override;
  std::optional<int> context_window;
};

absl::StatusOr<ValidatedRunPromptRequest> ValidateRunPromptRequest(const RunPromptRequest& request,
                                                                    const ServerRuntimeConfig& server_config);

}  // namespace slop::rpc::v1

#endif  // SLOP_RPC_REQUEST_VALIDATION_H_
