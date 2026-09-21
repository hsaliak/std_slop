#ifndef SLOP_MCP_TOKEN_STORE_INTERNAL_H_
#define SLOP_MCP_TOKEN_STORE_INTERNAL_H_

#include <cstddef>

#include <sys/types.h>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace slop::mcp::token_store_internal {

using WriteFunction =
    absl::FunctionRef<ssize_t(int, const void*, size_t)>;

absl::Status WriteAll(int fd, absl::string_view content,
                      WriteFunction write_function);

}  // namespace slop::mcp::token_store_internal

#endif  // SLOP_MCP_TOKEN_STORE_INTERNAL_H_
