#include "mcp/gateway/result.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "absl/status/status.h"

#include "core/json_utils.h"

namespace slop::mcp::gateway {
namespace {

constexpr std::size_t kMaxCompletionErrorBytes = 4096;

std::string ToolErrorMessage(const ToolCallResult& result, std::size_t max_bytes) {
  std::string message;
  for (const nlohmann::json& item : result.content) {
    const auto text = json_get<std::string>(item, "text");
    if (!text.has_value()) continue;
    if (!message.empty()) message.push_back('\n');
    const std::size_t remaining = max_bytes > message.size() ? max_bytes - message.size() : 0;
    message.append(text->data(), std::min(text->size(), remaining));
    if (message.size() >= max_bytes) break;
  }
  if (message.empty()) message = "downstream tool reported an error";
  return message;
}

}  // namespace

std::string BoundCompletionError(std::string error) {
  if (error.size() > kMaxCompletionErrorBytes) error.resize(kMaxCompletionErrorBytes);
  return error;
}

absl::StatusOr<NormalizedToolResult> NormalizeToolResult(const ToolCallResult& result, std::size_t max_bytes) {
  if (max_bytes == 0) return absl::InvalidArgumentError("result size limit must be positive");
  if (result.kind == ToolResultKind::kInputRequired) {
    return NormalizedToolResult{false, nullptr, "continuation", "downstream tool requires unsupported continuation"};
  }
  if (result.is_error) {
    return NormalizedToolResult{false, nullptr, "tool", ToolErrorMessage(result, max_bytes)};
  }
  nlohmann::json normalized = {{"content", result.content}, {"isError", false}};
  if (result.structured_content.has_value()) normalized["structuredContent"] = *result.structured_content;
  if (!result.meta.empty()) normalized["meta"] = result.meta;
  const std::string serialized = json_dump(normalized);
  if (serialized.size() > max_bytes) return absl::ResourceExhaustedError("downstream result exceeds size limit");
  return NormalizedToolResult{true, std::move(normalized), "", ""};
}

}  // namespace slop::mcp::gateway
