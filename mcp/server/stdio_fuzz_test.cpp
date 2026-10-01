#include <sstream>
#include <string>

#include "gtest/gtest.h"

#include "core/json_utils.h"
#include "fuzztest/fuzztest.h"
#include "mcp/json_rpc.h"
#include "mcp/server/stdio.h"

namespace slop::mcp::server {
namespace {

ImplementationInfo Identity() {
  ImplementationInfo identity;
  identity.name = "stdio-fuzz";
  identity.version = "1";
  return identity;
}

void FramingNeverCrashes(const std::string& raw) {
  auto server = Server::Create(Identity());
  ASSERT_TRUE(server.ok());
  StdioOptions options;
  options.max_input_bytes = 256;
  std::istringstream input(raw);
  std::ostringstream output;
  std::ostringstream diagnostics;
  const auto status = RunStdio(*server, input, output, diagnostics, options);

  // Reference framing: only complete lines within the bound reach dispatch.
  size_t offset = 0;
  std::string expected_output;
  absl::StatusCode expected_code = absl::StatusCode::kOk;
  while (offset < raw.size()) {
    const auto newline = raw.find('\n', offset);
    const auto length = (newline == std::string::npos ? raw.size() : newline) - offset;
    if (length > options.max_input_bytes) {
      expected_code = absl::StatusCode::kResourceExhausted;
      break;
    }
    if (newline == std::string::npos) {
      expected_code = absl::StatusCode::kDataLoss;
      break;
    }
    const auto reply = server->Dispatch(raw.substr(offset, length));
    if (reply) expected_output += json_dump(*reply) + "\n";
    offset = newline + 1;
  }
  EXPECT_EQ(status.code(), expected_code);
  EXPECT_EQ(output.str(), expected_output);
  EXPECT_EQ(diagnostics.str().empty(), status.ok());
  std::istringstream responses(output.str());
  std::string line;
  while (std::getline(responses, line)) {
    const auto reply = ParseJsonRpcMessage(line);
    ASSERT_TRUE(reply.ok());
    EXPECT_TRUE(ParseJsonRpcResponse(*reply).ok());
  }
}
FUZZ_TEST(StdioFuzzTest, FramingNeverCrashes);

void OversizedOrTruncatedCallsNeverExecute(const std::string& text) {
  int calls = 0;
  ToolRegistration tool;
  tool.definition.name = "echo";
  tool.definition.input_schema = {
      {"type", "object"}, {"required", {"text"}}, {"properties", {{"text", {{"type", "string"}}}}}};
  tool.handler = [&calls](const nlohmann::json&) -> absl::StatusOr<ToolCallResult> {
    ++calls;
    return ToolCallResult{};
  };
  auto server = Server::Create(Identity(), {tool});
  ASSERT_TRUE(server.ok());
  const nlohmann::json request = {{"jsonrpc", "2.0"},
                                  {"id", 1},
                                  {"method", "tools/call"},
                                  {"params",
                                   {{"name", "echo"},
                                    {"arguments", {{"text", text.substr(0, 1024)}}},
                                    {"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}}};
  const auto raw = json_dump(request);
  StdioOptions options;
  options.max_input_bytes = raw.size() - 1;
  std::istringstream oversized(raw + "\n" + raw + "\n");
  std::ostringstream output;
  std::ostringstream diagnostics;
  EXPECT_EQ(RunStdio(*server, oversized, output, diagnostics, options).code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  options.max_input_bytes = raw.size();
  std::istringstream truncated(raw);
  EXPECT_EQ(RunStdio(*server, truncated, output, diagnostics, options).code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(output.str().empty());
  std::istringstream complete(raw + "\n");
  EXPECT_TRUE(RunStdio(*server, complete, output, diagnostics, options).ok());
  EXPECT_EQ(calls, 1);
}
FUZZ_TEST(StdioFuzzTest, OversizedOrTruncatedCallsNeverExecute);

TEST(StdioFuzzTest, RegressionSeeds) {
  for (const std::string raw : {"", "\n", "{\n[]\n", "unterminated", "\r\n", "null\n", "\n\n"}) {
    FramingNeverCrashes(raw);
  }
  FramingNeverCrashes(std::string(257, 'x') + "\n");
  OversizedOrTruncatedCallsNeverExecute("hello\nworld");
  OversizedOrTruncatedCallsNeverExecute("");
}

}  // namespace
}  // namespace slop::mcp::server
