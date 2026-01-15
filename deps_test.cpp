#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <gtest/gtest.h>
#include <iostream>

TEST(DepsTest, JsonWorks) {
    nlohmann::json j = {{"test", "ok"}};
    EXPECT_EQ(j["test"], "ok");
}

TEST(DepsTest, CurlWorks) {
    CURL* curl = curl_easy_init();
    ASSERT_NE(curl, nullptr);
    curl_easy_cleanup(curl);
}
