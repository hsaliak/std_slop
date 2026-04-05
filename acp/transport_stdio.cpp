
#include "acp/transport_stdio.h"

#include <utility>

#include "core/json_utils.h"

namespace slop::acp {

std::optional<std::string> StdioTransport::ReadLine() {
  std::string line;
  if (!std::getline(*in_, line)) {
    return std::nullopt;
  }
  return line;
}

void StdioTransport::WriteJson(const nlohmann::json& payload) {
  (*out_) << json_dump(payload) << '\n';
  out_->flush();
}

}  // namespace slop::acp