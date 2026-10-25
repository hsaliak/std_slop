#ifndef SLOP_TOOLS_EDIT_ENGINE_H_
#define SLOP_TOOLS_EDIT_ENGINE_H_

#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace slop {

struct TextEditResult {
  std::string content;
  nlohmann::json changes = nlohmann::json::array();
  std::size_t edit_count = 0;
  std::size_t bytes_before = 0;
};

absl::Status ValidateTextEdits(const nlohmann::json& edits);
absl::StatusOr<TextEditResult> ApplyTextEdits(absl::string_view original, const nlohmann::json& edits);

}  // namespace slop

#endif  // SLOP_TOOLS_EDIT_ENGINE_H_
