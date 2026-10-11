#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "fuzztest/fuzztest.h"
#include "mcp/client/oauth_client.h"

namespace slop::mcp {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  explicit FakeHttpClient(std::string response_body) : response_body_(std::move(response_body)) {}

  absl::StatusOr<HttpResponse> PostOnceWithResponse(const std::string&, const std::string&,
                                                    const std::vector<std::string>&) override {
    return HttpResponse{200, response_body_, {}};
  }

 private:
  std::string response_body_;
};

OAuthClientConfig Config() {
  OAuthClientConfig config;
  config.client_id = "client";
  config.authorization_endpoint = "https://auth.example/authorize";
  config.token_endpoint = "https://auth.example/token";
  config.issuer = "https://auth.example";
  config.resource = "https://api.example/mcp";
  return config;
}

void TokenResponseParserNeverCrashes(const std::string& body) {
  FakeHttpClient http(body);
  const OAuthClientConfig config = Config();

  const auto result = ExchangeAuthorizationCode(&http, config, "code", "verifier");

  if (result.ok()) {
    EXPECT_FALSE(result->access_token.empty());
    EXPECT_EQ(result->issuer, config.issuer);
    EXPECT_EQ(result->resource, config.resource);
  }
}

void CallbackParserNeverCrashes(const std::string& callback) {
  const auto result = ExtractAuthorizationCodeFromCallback(callback, "expected-state", "https://auth.example",
                                                           "http://127.0.0.1/callback", true);
  if (result.ok()) EXPECT_FALSE(result->empty());
}

FUZZ_TEST(OAuthClientFuzzTest, TokenResponseParserNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple(R"({"access_token":"access","token_type":"Bearer"})"),
        std::make_tuple(R"({"error":"invalid_grant","error_description":"denied"})"),
        std::make_tuple("not-json"),
    });

FUZZ_TEST(OAuthClientFuzzTest, CallbackParserNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string>>{
        std::make_tuple("http://127.0.0.1/callback?code=abc&state=expected-state&iss=https%3A%2F%2Fauth.example"),
        std::make_tuple("http://127.0.0.1/callback?code=abc&state=wrong&iss=https%3A%2F%2Fauth.example"),
        std::make_tuple("http://127.0.0.1/callback?code=%GG&state=expected-state"),
        std::make_tuple("http://127.0.0.1/callback?error=&code=abc&state=expected-state"),
        std::make_tuple("http://127.0.0.1/callback?error=access_denied&error=&state=expected-state"),
        std::make_tuple("http://127.0.0.1/callback?code=abc&state=expected-state&error"),
    });

}  // namespace
}  // namespace slop::mcp
