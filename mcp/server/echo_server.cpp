#include <csignal>
#include <iostream>
#include <string>

#include "core/json_utils.h"
#include "mcp/server/stdio.h"
#include "mcp/server/tool_utils.h"

int main(int argc, char** argv) {
  if (argc != 1) {
    std::cerr << "usage: echo_server\n";
    return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 2;
  }
  // Let the transport report a closed stdout pipe as an I/O failure.
  std::signal(SIGPIPE, SIG_IGN);
  std::ios::sync_with_stdio(false);
  // Responses must be flushed by RunStdio, not implicitly by reads from cin.
  std::cin.tie(nullptr);

  slop::mcp::ImplementationInfo identity;
  identity.name = "slop-echo-server";
  identity.version = "1.0";
  auto echo = slop::mcp::server::MakeToolRegistration(
      "echo", "Return the supplied text without side effects",
      {{"type", "object"},
       {"properties", {{"text", {{"type", "string"}}}}},
       {"required", {"text"}},
       {"additionalProperties", false}},
      {{"readOnlyHint", true}},
      [](const nlohmann::json& arguments) -> absl::StatusOr<slop::mcp::ToolCallResult> {
        return slop::mcp::server::MakeToolCallResult(
            slop::json_get_or(arguments, "text", std::string{}), arguments);
      });
  echo.definition.output_schema = echo.definition.input_schema;
  const auto server = slop::mcp::server::Server::Create(identity, {echo});
  if (!server.ok()) {
    std::cerr << "echo server: " << server.status().message() << '\n';
    return 1;
  }
  const auto status = slop::mcp::server::RunStdio(*server, std::cin, std::cout, std::cerr);
  return status.ok() ? 0 : 1;
}
