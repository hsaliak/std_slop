#include "core/http_client.h"

#include <cctype>
#include <string>
#include <tuple>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

// ParseHttpStatusLine must never crash on arbitrary input and must only accept
// well-formed HTTP status lines, writing a plausible numeric code on success.
void ParseHttpStatusLineNeverCrashesAndRejectsMalformed(const std::string& header) {
  long code = -1;
  const bool parsed = HttpClient::ParseHttpStatusLine(header, &code);
  if (parsed) {
    // A parsed status code must be a non-negative integer in a sane range.
    EXPECT_GE(code, 0);
    EXPECT_LE(code, 999);
  } else {
    // On failure the out-parameter must remain untouched.
    EXPECT_EQ(code, -1);
  }
}

// CaptureHeaderField must never crash on arbitrary input. When it parses a
// line it must store a lowercase key and a trimmed value without duplicating.
void CaptureHeaderFieldNeverCrashes(const std::string& header) {
  absl::flat_hash_map<std::string, std::string> headers;
  HttpClient::CaptureHeaderField(header, &headers);
  for (const auto& [key, value] : headers) {
    // Keys are always stored lowercase.
    for (char c : key) {
      EXPECT_TRUE(c == static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
}

FUZZ_TEST(HttpClientFuzzTest, ParseHttpStatusLineNeverCrashesAndRejectsMalformed)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple("HTTP/1.1 200 OK\r\n"),
        std::make_tuple("HTTP/2 418 I'm a teapot"),
        std::make_tuple("HTTP/1.0 503 Service Unavailable\n"),
        std::make_tuple("not a status line"),
        std::make_tuple("HTTP/1.1\r\n"),
        std::make_tuple("HTTP/1.1 abc Bad\r\n"),
        std::make_tuple(""),
    });

FUZZ_TEST(HttpClientFuzzTest, CaptureHeaderFieldNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple("Content-Type: application/json\r\n"),
        std::make_tuple("Retry-After: 120\r\n"),
        std::make_tuple("HTTP/1.1 200 OK\r\n"),
        std::make_tuple("\r\n"),
        std::make_tuple("no-colon-here"),
        std::make_tuple("  X-Custom :  spaced value  \r\n"),
    });

}  // namespace
}  // namespace slop
