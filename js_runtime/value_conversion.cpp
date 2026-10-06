#include "js_runtime/value_conversion.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace slop::js_runtime {
namespace {

constexpr std::size_t kMaxDepth = 64;
constexpr std::size_t kMaxNodes = 100000;
constexpr std::size_t kMaxArrayLength = 100000;

class Converter {
 public:
  Converter(JSContext* context, const JsPrototypes& prototypes, std::size_t max_bytes)
      : context_(context), prototypes_(prototypes), max_bytes_(max_bytes) {}

  absl::StatusOr<nlohmann::json> Convert(JSValueConst value) { return ConvertValue(value, 0); }

 private:
  absl::StatusOr<nlohmann::json> ConvertValue(JSValueConst value, std::size_t depth) {
    if (++nodes_ > kMaxNodes) return absl::ResourceExhaustedError("JSON value has too many nodes");
    if (depth > kMaxDepth) return absl::ResourceExhaustedError("JSON value exceeds maximum depth");
    if (JS_IsNull(value)) return nullptr;
    if (JS_IsBool(value)) return JS_ToBool(context_, value) != 0;
    if (JS_IsNumber(value)) {
      double number = 0;
      if (JS_ToFloat64(context_, &number, value) < 0 || !std::isfinite(number)) {
        return absl::InvalidArgumentError("JSON numbers must be finite");
      }
      return nlohmann::json(number);
    }
    if (JS_IsString(value)) {
      absl::StatusOr<std::string> string = StringValue(value);
      if (!string.ok()) return string.status();
      return nlohmann::json(*string);
    }
    if (JS_IsUndefined(value) || JS_IsFunction(context_, value) || JS_IsBigInt(value) || JS_IsSymbol(value)) {
      return absl::InvalidArgumentError("value is not supported in JSON");
    }
    if (!JS_IsObject(value)) return absl::InvalidArgumentError("value is not supported in JSON");
    if (JS_IsProxy(value)) return absl::InvalidArgumentError("Proxy values are not supported in JSON");
    for (JSValueConst ancestor : ancestors_) {
      if (JS_IsStrictEqual(context_, value, ancestor))
        return absl::InvalidArgumentError("cyclic values are not supported in JSON");
    }
    ancestors_.push_back(value);
    absl::Cleanup pop_ancestor = [this] { ancestors_.pop_back(); };
    if (JS_IsArray(value)) return ConvertArray(value, depth);
    return ConvertObject(value, depth);
  }

  absl::StatusOr<nlohmann::json> ConvertArray(JSValueConst value, std::size_t depth) {
    absl::Status status = CheckPrototype(value, prototypes_.array);
    if (!status.ok()) return status;
    JSAtom length_atom = JS_NewAtom(context_, "length");
    absl::Cleanup free_length_atom = [this, length_atom] { JS_FreeAtom(context_, length_atom); };
    JSPropertyDescriptor length_descriptor{};
    if (JS_GetOwnProperty(context_, &length_descriptor, value, length_atom) <= 0 ||
        (length_descriptor.flags & JS_PROP_GETSET) != 0) {
      FreeDescriptor(length_descriptor);
      return absl::InvalidArgumentError("array length is not a data property");
    }
    std::uint32_t length = 0;
    const int length_status = JS_ToUint32(context_, &length, length_descriptor.value);
    FreeDescriptor(length_descriptor);
    if (length_status < 0 || length > kMaxArrayLength) {
      return absl::ResourceExhaustedError("array exceeds maximum length");
    }

    nlohmann::json result = nlohmann::json::array();
    for (std::uint32_t index = 0; index < length; ++index) {
      JSAtom atom = JS_NewAtomUInt32(context_, index);
      JSPropertyDescriptor descriptor{};
      const int found = JS_GetOwnProperty(context_, &descriptor, value, atom);
      JS_FreeAtom(context_, atom);
      if (found <= 0) {
        FreeDescriptor(descriptor);
        return absl::InvalidArgumentError("sparse arrays are not supported in JSON");
      }
      if ((descriptor.flags & JS_PROP_GETSET) != 0) {
        FreeDescriptor(descriptor);
        return absl::InvalidArgumentError("accessor properties are not supported in JSON");
      }
      absl::StatusOr<nlohmann::json> item = ConvertValue(descriptor.value, depth + 1);
      FreeDescriptor(descriptor);
      if (!item.ok()) return item.status();
      result.push_back(std::move(*item));
    }
    absl::Status properties_status = CheckNoExtraArrayProperties(value, length);
    if (!properties_status.ok()) return properties_status;
    return result;
  }

  absl::Status CheckNoExtraArrayProperties(JSValueConst value, std::uint32_t length) {
    JSPropertyEnum* properties = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context_, &properties, &count, value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY | JS_GPN_SET_ENUM) < 0) {
      return absl::InvalidArgumentError("failed to inspect array properties");
    }
    absl::Cleanup free_properties = [this, properties, count] { JS_FreePropertyEnum(context_, properties, count); };
    for (std::uint32_t i = 0; i < count; ++i) {
      JSValue key = JS_AtomToValue(context_, properties[i].atom);
      absl::StatusOr<std::string> key_text = StringValue(key);
      JS_FreeValue(context_, key);
      if (!key_text.ok()) return key_text.status();
      if (*key_text == "length") continue;
      if (key_text->empty() || (key_text->size() > 1 && key_text->front() == '0')) {
        return absl::InvalidArgumentError("extra array properties are not supported in JSON");
      }
      std::uint64_t index = 0;
      for (char digit : *key_text) {
        if (digit < '0' || digit > '9')
          return absl::InvalidArgumentError("extra array properties are not supported in JSON");
        if (length == 0 || index > (length - 1) / 10) {
          return absl::InvalidArgumentError("extra array properties are not supported in JSON");
        }
        index = index * 10 + static_cast<std::uint64_t>(digit - '0');
        if (index >= length) return absl::InvalidArgumentError("extra array properties are not supported in JSON");
      }
    }
    return absl::OkStatus();
  }

  absl::StatusOr<nlohmann::json> ConvertObject(JSValueConst value, std::size_t depth) {
    absl::Status status = CheckPrototype(value, prototypes_.object);
    if (!status.ok()) return status;
    JSPropertyEnum* properties = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context_, &properties, &count, value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY | JS_GPN_SET_ENUM) < 0) {
      return absl::InvalidArgumentError("failed to inspect object properties");
    }
    absl::Cleanup free_properties = [this, properties, count] { JS_FreePropertyEnum(context_, properties, count); };
    nlohmann::json result = nlohmann::json::object();
    for (std::uint32_t i = 0; i < count; ++i) {
      JSPropertyDescriptor descriptor{};
      if (JS_GetOwnProperty(context_, &descriptor, value, properties[i].atom) <= 0) {
        FreeDescriptor(descriptor);
        return absl::InvalidArgumentError("failed to read object property descriptor");
      }
      if ((descriptor.flags & JS_PROP_GETSET) != 0) {
        FreeDescriptor(descriptor);
        return absl::InvalidArgumentError("accessor properties are not supported in JSON");
      }
      const char* raw_key = JS_AtomToCString(context_, properties[i].atom);
      if (raw_key == nullptr) {
        FreeDescriptor(descriptor);
        return absl::InvalidArgumentError("object key is not valid text");
      }
      const std::string key(raw_key);
      JS_FreeCString(context_, raw_key);
      status = AddBytes(key.size());
      if (!status.ok()) {
        FreeDescriptor(descriptor);
        return status;
      }
      absl::StatusOr<nlohmann::json> item = ConvertValue(descriptor.value, depth + 1);
      FreeDescriptor(descriptor);
      if (!item.ok()) return item.status();
      result[key] = std::move(*item);
    }
    return result;
  }

  absl::Status CheckPrototype(JSValueConst value, JSValueConst expected) {
    JSValue prototype = JS_GetPrototype(context_, value);
    if (JS_IsException(prototype)) return absl::InvalidArgumentError("failed to inspect JSON object prototype");
    const bool allowed = JS_IsNull(prototype) || JS_IsStrictEqual(context_, prototype, expected);
    JS_FreeValue(context_, prototype);
    if (!allowed) return absl::InvalidArgumentError("custom object prototypes are not supported in JSON");
    return absl::OkStatus();
  }

  absl::StatusOr<std::string> StringValue(JSValueConst value) {
    size_t length = 0;
    const char* raw = JS_ToCStringLen(context_, &length, value);
    if (raw == nullptr) return absl::InvalidArgumentError("string conversion failed");
    absl::Cleanup free_raw = [this, raw] { JS_FreeCString(context_, raw); };
    absl::Status status = AddBytes(length);
    if (!status.ok()) return status;
    return std::string(raw, length);
  }

  absl::Status AddBytes(std::size_t bytes) {
    if (bytes > max_bytes_ - std::min(max_bytes_, bytes_)) {
      return absl::ResourceExhaustedError("JSON value exceeds size limit");
    }
    bytes_ += bytes;
    return absl::OkStatus();
  }

  void FreeDescriptor(const JSPropertyDescriptor& descriptor) {
    JS_FreeValue(context_, descriptor.value);
    JS_FreeValue(context_, descriptor.getter);
    JS_FreeValue(context_, descriptor.setter);
  }

  JSContext* context_;
  const JsPrototypes& prototypes_;
  std::size_t max_bytes_;
  std::size_t bytes_ = 0;
  std::size_t nodes_ = 0;
  std::vector<JSValueConst> ancestors_;
};

}  // namespace

absl::StatusOr<nlohmann::json> JsonFromJs(JSContext* context, JSValueConst value, const JsPrototypes& prototypes,
                                          std::size_t max_bytes) {
  if (context == nullptr) return absl::InvalidArgumentError("QuickJS context is null");
  return Converter(context, prototypes, max_bytes).Convert(value);
}

}  // namespace slop::js_runtime
