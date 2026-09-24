#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

#include "core/http_client.h"
#include "fuzztest/fuzztest.h"
#include "mcp/client/modern_http.h"

namespace slop::mcp::v2026_07_28 {
namespace {

class FuzzHttpClient : public HttpClient {
 public:
  explicit FuzzHttpClient(HttpResponse response) : response_(std::move(response)) {}

  absl::StatusOr<HttpResponse> PostOnceStreamWithResponse(const std::string&, const std::string&,
                                                          const std::vector<std::string>&, absl::Duration, size_t,
                                                          ChunkCallback on_chunk) override {
    const absl::Status status = on_chunk(response_.body);
    if (!status.ok()) return status;
    return response_;
  }

 private:
  HttpResponse response_;
};

void ModernHttpResponseNeverCrashes(const std::string& body, bool sse, int status_offset) {
  HttpResponse response;
  response.status_code = 200 + status_offset % 400;
  response.body = body;
  response.headers["content-type"] = sse ? "text/event-stream" : "application/json";
  FuzzHttpClient http(std::move(response));
  HttpExchangeOptions options;
  options.endpoint_url = "https://example.test/mcp";
  options.max_response_bytes = 4096;
  options.max_messages = 32;
  HttpExchange exchange(options, &http);
  Request request;
  request.id = std::string("fuzz-id");
  request.method = "server/discover";
  request.context.client_info.name = "fuzzer";
  request.context.client_info.version = "1";
  (void)exchange.Execute(request);
}
FUZZ_TEST(ModernHttpFuzzTest, ModernHttpResponseNeverCrashes);

TEST(ModernHttpFuzzTest, RegressionSeeds) {
  ModernHttpResponseNeverCrashes(R"({"jsonrpc":"2.0","id":"fuzz-id","result":{}})", false, 0);
  ModernHttpResponseNeverCrashes("data: {\"jsonrpc\":\"2.0\",\"id\":\"fuzz-id\",\"result\":{}}\n\n", true, 0);
  ModernHttpResponseNeverCrashes("data: {", true, 200);
}

}  // namespace
}  // namespace slop::mcp::v2026_07_28
