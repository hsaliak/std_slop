#include <iostream>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "core/http_client.h"
#include "core/json_utils.h"
#include "mcp/client.h"
#include "mcp/types.h"
#include "nlohmann/json.hpp"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: call_tool_example <mcp-endpoint-url> <tool-name> <arguments-json-object>\n";
    return 2;
  }

  auto arguments = slop::json_parse(argv[3]);
  if (!arguments.has_value() || !arguments->is_object()) {
    std::cerr << "arguments-json-object must be a JSON object\n";
    return 2;
  }

  slop::mcp::StreamableHttpConfig config;
  config.endpoint_url = argv[1];

  slop::mcp::InitializeOptions options;
  options.client_info.name = "slop-call-tool-example";
  options.client_info.version = "1.0";

  slop::HttpClient http_client;
  auto session = slop::mcp::ConnectStreamableHttp(config, options, &http_client);
  if (!session.ok()) {
    std::cerr << session.status() << "\n";
    return 1;
  }

  auto result = (*session)->CallTool(argv[2], *arguments);
  if (!result.ok()) {
    std::cerr << result.status() << "\n";
    return 1;
  }

  nlohmann::json output = {{"content", result->content}, {"isError", result->is_error}};
  if (!result->structured_content.empty()) output["structuredContent"] = result->structured_content;
  std::cout << slop::json_dump(output, 2) << "\n";
  return result->is_error ? 1 : 0;
}
