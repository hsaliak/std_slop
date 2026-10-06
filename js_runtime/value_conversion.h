#ifndef SLOP_JS_RUNTIME_VALUE_CONVERSION_H_
#define SLOP_JS_RUNTIME_VALUE_CONVERSION_H_

#include <cstddef>

#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

#include "quickjs.h"

namespace slop::js_runtime {

struct JsPrototypes {
  JSValue object;
  JSValue array;
};

absl::StatusOr<nlohmann::json> JsonFromJs(JSContext* context, JSValueConst value, const JsPrototypes& prototypes,
                                          std::size_t max_bytes);

}  // namespace slop::js_runtime

#endif  // SLOP_JS_RUNTIME_VALUE_CONVERSION_H_
