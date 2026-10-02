#include <csignal>
#include <iostream>
#include <string>

#include "core/json_utils.h"
#include "mcp/server/stdio.h"

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
  slop::mcp::server::ToolRegistration echo;
  echo.definition.name = "echo";
  echo.definition.description = "Return the supplied text without side effects";
  echo.definition.input_schema = {{"type", "object"},
                                  {"properties", {{"text", {{"type", "string"}}}}},
                                  {"required", {"text"}},
                                  {"additionalProperties", false}};
  echo.definition.output_schema = echo.definition.input_schema;
  echo.definition.annotations = {{"readOnlyHint", true}};
  echo.handler = [](const nlohmann::json& arguments) -> absl::StatusOr<slop::mcp::ToolCallResult> {
    slop::mcp::ToolCallResult result;
    result.content.push_back({{"type", "text"}, {"text", slop::json_get_or(arguments, "text", std::string{})}});
    result.structured_content = arguments;
    return result;
  };
  const auto server = slop::mcp::server::Server::Create(identity, {echo});
  if (!server.ok()) {
    std::cerr << "echo server: " << server.status().message() << '\n';
    return 1;
  }
  const auto status = slop::mcp::server::RunStdio(*server, std::cin, std::cout, std::cerr);
  return status.ok() ? 0 : 1;
}
