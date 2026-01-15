#include "http_client.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <nlohmann/json.hpp>

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

}  // namespace slop
