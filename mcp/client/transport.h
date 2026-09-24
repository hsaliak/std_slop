#ifndef SLOP_MCP_TRANSPORT_H_
#define SLOP_MCP_TRANSPORT_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "nlohmann/json.hpp"

namespace slop::mcp {

class Transport {
 public:
  virtual ~Transport() = default;

  virtual absl::Status Start() = 0;
  virtual absl::Status Send(const nlohmann::json& message) = 0;
  virtual absl::StatusOr<nlohmann::json> Receive(absl::Duration timeout) = 0;
  virtual void SetProtocolVersion(absl::string_view) {}
  virtual absl::Status Close() = 0;
};

}  // namespace slop::mcp

#endif  // SLOP_MCP_TRANSPORT_H_
