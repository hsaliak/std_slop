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
  if (argc != 2) {
    std::cerr << "usage: list_tools_example <mcp-endpoint-url>\n";
    return 2;
  }

  slop::mcp::StreamableHttpConfig config;
  config.endpoint_url = argv[1];

  slop::mcp::InitializeOptions options;
  options.client_info.name = "slop-list-tools-example";
  options.client_info.version = "1.0";

  slop::HttpClient http_client;
  auto session = slop::mcp::ConnectStreamableHttp(config, options, &http_client);
  if (!session.ok()) {
    std::cerr << session.status() << "\n";
    return 1;
  }

  auto tools = (*session)->ListTools();
  if (!tools.ok()) {
    std::cerr << tools.status() << "\n";
    return 1;
  }

  nlohmann::json output = nlohmann::json::array();
  for (const auto& tool : *tools) {
    nlohmann::json item = {{"name", tool.name}, {"inputSchema", tool.input_schema}};
    if (tool.title.has_value()) item["title"] = *tool.title;
    if (tool.description.has_value()) item["description"] = *tool.description;
    if (!tool.output_schema.empty()) item["outputSchema"] = tool.output_schema;
    if (!tool.annotations.empty()) item["annotations"] = tool.annotations;
    output.push_back(item);
  }

  std::cout << slop::json_dump(output, 2) << "\n";
  return 0;
}
