#include "http_client.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace sentinel {

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

// Note: Testing real network calls requires an API key and internet access.
// This test is intended to be run manually or in an environment with the key.
TEST(HttpClientTest, GeminiLiveTest) {
    const char* api_key = std::getenv("GOOGLE_API_KEY");
    if (!api_key) {
        GTEST_SKIP() << "Skipping GeminiLiveTest: GOOGLE_API_KEY not set";
    }

    HttpClient client;
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent?key=" + std::string(api_key);
    
    nlohmann::json body = {
        {"contents", {
            {{"role", "user"}, {"parts", {{{"text", "Say hello in one word."}}}}}
        }}
    };

    std::vector<std::string> headers = {
        "Content-Type: application/json"
    };

    auto result = client.Post(url, body.dump(), headers);
    ASSERT_TRUE(result.ok()) << result.status().message();
    
    nlohmann::json response = nlohmann::json::parse(*result);
    // Basic validation of Gemini response structure
    ASSERT_TRUE(response.contains("candidates"));
    std::cout << "Gemini response: " << response.dump(2) << std::endl;
}

}  // namespace sentinel
