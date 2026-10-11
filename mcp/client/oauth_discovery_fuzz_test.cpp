#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"

#include "fuzztest/fuzztest.h"
#include "mcp/client/oauth_discovery.h"

namespace slop::mcp {
namespace {

class DiscoveryHttpClient : public HttpClient {
 public:
  explicit DiscoveryHttpClient(int response_code, std::string challenge, std::string resource_metadata,
                               std::string authorization_metadata)
      : response_code_(response_code),
        challenge_(std::move(challenge)),
        resource_metadata_(std::move(resource_metadata)),
        authorization_metadata_(std::move(authorization_metadata)) {}

  absl::StatusOr<HttpResponse> PostOnceWithResponse(const std::string&, const std::string&,
                                                    const std::vector<std::string>&) override {
    return HttpResponse{response_code_, "", {{"www-authenticate", challenge_}}};
  }

  absl::StatusOr<std::string> GetOnce(const std::string& url, const std::vector<std::string>&) override {
    if (url.find("oauth-authorization-server") != std::string::npos ||
        url.find("openid-configuration") != std::string::npos) {
      return authorization_metadata_;
    }
    return resource_metadata_;
  }

 private:
  int response_code_;
  std::string challenge_;
  std::string resource_metadata_;
  std::string authorization_metadata_;
};

void DiscoveryHandlesUntrustedMetadataWithoutCrashing(const std::string& challenge,
                                                      const std::string& resource_metadata,
                                                      const std::string& authorization_metadata,
                                                      const std::string& selected_issuer,
                                                      const std::string& resource_metadata_url, int response_code) {
  constexpr char kResourceUrl[] = "https://resource.example/mcp";
  DiscoveryHttpClient http(response_code, challenge, resource_metadata, authorization_metadata);
  OAuthDiscoveryOptions options;
  options.selected_authorization_server = selected_issuer;
  options.resource_metadata_url = resource_metadata_url;

  const auto discovered = DiscoverOAuthEndpoints(&http, kResourceUrl, options);
  if (!discovered.ok()) return;

  EXPECT_EQ(discovered->resource, kResourceUrl);
  EXPECT_FALSE(discovered->issuer.empty());
  EXPECT_FALSE(discovered->authorization_endpoint.empty());
  EXPECT_FALSE(discovered->token_endpoint.empty());
  if (!selected_issuer.empty()) EXPECT_EQ(discovered->issuer, selected_issuer);
}

FUZZ_TEST(McpOAuthDiscoveryFuzzTest, DiscoveryHandlesUntrustedMetadataWithoutCrashing)
    .WithSeeds(std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string, int>>{
        std::make_tuple(
            R"(Bearer resource_metadata="https://resource.example/.well-known/oauth-protected-resource")",
            R"({"resource":"https://resource.example/mcp","authorization_servers":["https://auth.example"]})",
            R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","code_challenge_methods_supported":["S256"]})",
            "", "", 401),
        std::make_tuple(
            "", R"({"resource":"https://resource.example/mcp","authorization_servers":["https://auth.example"]})",
            R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/authorize","token_endpoint":"https://auth.example/token","code_challenge_methods_supported":["S256"]})",
            "", "", 200),
        std::make_tuple("Bearer realm=example", "{", "not-json", "https://auth.example",
                        "https://resource.example/meta", 401),
    });

}  // namespace
}  // namespace slop::mcp
