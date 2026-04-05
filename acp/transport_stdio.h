
#ifndef SLOP_ACP_TRANSPORT_STDIO_H_
#define SLOP_ACP_TRANSPORT_STDIO_H_

#include <istream>
#include <optional>
#include <ostream>
#include <string>

#include "nlohmann/json.hpp"

namespace slop::acp {

class StdioTransport {
 public:
  StdioTransport(std::istream* in, std::ostream* out) : in_(in), out_(out) {}

  std::optional<std::string> ReadLine();
  void WriteJson(const nlohmann::json& payload);

 private:
  std::istream* in_;
  std::ostream* out_;
};

}  // namespace slop::acp

#endif  // SLOP_ACP_TRANSPORT_STDIO_H_