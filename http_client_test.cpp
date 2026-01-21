#include "http_client.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>

namespace slop {

TEST(HttpClientTest, PostInit) {
    HttpClient client;
    // Basic test to ensure it doesn't crash
}

TEST(HttpClientTest, GetError) {
    HttpClient client;
    // Should fail on a non-existent local port or invalid URL
    auto res = client.Get("http://localhost:1", {});
    EXPECT_FALSE(res.ok());
}

TEST(HttpClientTest, HttpsSupport) {
    // Check if libcurl has SSL support enabled
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->features & CURL_VERSION_SSL) << "libcurl was built without SSL support";

    HttpClient client;
    auto res = client.Get("https://www.google.com", {});
    // If protocol is unsupported, it will return an InternalError with "Unsupported protocol"
    if (!res.ok()) {
        EXPECT_FALSE(res.status().message().find("Unsupported protocol") != std::string::npos) 
            << "HTTPS protocol is not supported in the current libcurl build: " << res.status().message();
    }
}

TEST(HttpClientTest, PostBasic) {
    HttpClient client;
    // We don't have a mock server, but we can at least check if it handles 
    // a non-existent endpoint correctly without crashing.
    auto res = client.Post("http://localhost:1", "{\"test\":true}", {"Content-Type: application/json"});
    EXPECT_FALSE(res.ok());
}

}  // namespace slop
