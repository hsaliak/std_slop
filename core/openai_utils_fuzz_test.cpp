
#include "core/openai_utils.h"

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

class FakeHttpClient : public HttpClient {
 public:
  explicit FakeHttpClient(std::vector<std::string> responses)
      : HttpClient(/*max_retries=*/0, /*initial_backoff_ms=*/1), responses_(std::move(responses)) {}

  absl::StatusOr<std::string> Get(const std::string& /*url*/, const std::vector<std::string>& /*headers*/) override {
    if (index_ >= responses_.size()) {
      return std::string("{}");
    }
    return responses_[index_++];
  }

 private:
  std::vector<std::string> responses_;
  size_t index_ = 0;
};

void BuildResponsesToolsNeverCrashes(const std::vector<std::string>& names, const std::vector<std::string>& schemas) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  const size_t n = std::min(names.size(), schemas.size());
  for (size_t i = 0; i < n; ++i) {
    Database::Tool t;
    t.name = names[i].empty() ? "tool_" + std::to_string(i) : names[i];
    t.description = "fuzz tool";
    t.json_schema = schemas[i];
    t.is_enabled = true;
    EXPECT_TRUE(db.RegisterTool(t).ok());
  }

  auto tools_or = db.GetTopLevelTools();
  ASSERT_TRUE(tools_or.ok());
  const nlohmann::json out = BuildOpenAiResponsesTools(*tools_or);
  EXPECT_TRUE(out.is_array());
  for (const auto& item : out) {
    EXPECT_TRUE(item.is_object());
    if (item.contains("parameters") && item["parameters"].is_object()) {
      const auto& p = item["parameters"];
      if (p.value("type", std::string()) == "array" && p.contains("items")) {
        EXPECT_TRUE(p["items"].is_object());
      }
    }
  }
}

void GetModelsRobustForArbitraryResponses(const std::vector<std::string>& response_pages, const std::string& base_url,
                                          const std::string& api_key) {
  std::vector<std::string> pages = response_pages;
  if (pages.empty()) {
    pages.push_back("{}");
  }
  FakeHttpClient client(pages);
  auto models_or = GetOpenAiModels(&client, base_url, api_key, "");
  if (!models_or.ok()) {
    EXPECT_FALSE(models_or.status().ok());
    return;
  }
  for (const auto& model : *models_or) {
    EXPECT_FALSE(model.id.empty());
    EXPECT_EQ(model.name, model.id);
  }
}

FUZZ_TEST(OpenAiUtilsFuzzTest, BuildResponsesToolsNeverCrashes)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::string>()),
                 fuzztest::VectorOf(fuzztest::Arbitrary<std::string>()));

FUZZ_TEST(OpenAiUtilsFuzzTest, GetModelsRobustForArbitraryResponses)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::string>()), fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop