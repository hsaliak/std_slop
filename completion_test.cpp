#include <gtest/gtest.h>
#include "completion.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

using json = nlohmann::json;

namespace sentinel {
std::vector<std::string> GetCompletionMatches(const std::string& text, 
                                               const std::string& line_until_cursor,
                                               const json& command_tree);
}

class CompletionTest : public ::testing::Test {
 protected:
  json tree;
  void SetUp() override {
    tree = {
      {"/context", {
        {"show", {}},
        {"drop", {}}
      }},
      {"/help", {}},
      {"/memo", {
        {"add", {}},
        {"list", {}}
      }}
    };
  }
};

TEST_F(CompletionTest, RootCommands) {
  auto matches = sentinel::GetCompletionMatches("/", "/", tree);
  EXPECT_EQ(matches.size(), 3);
  EXPECT_NE(std::find(matches.begin(), matches.end(), "/context"), matches.end());
  EXPECT_NE(std::find(matches.begin(), matches.end(), "/help"), matches.end());
}

TEST_F(CompletionTest, RootCommandsPrefix) {
  auto matches = sentinel::GetCompletionMatches("/c", "/c", tree);
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], "/context");
}

TEST_F(CompletionTest, SubCommands) {
  auto matches = sentinel::GetCompletionMatches("", "/context ", tree);
  EXPECT_EQ(matches.size(), 2);
  EXPECT_NE(std::find(matches.begin(), matches.end(), "show"), matches.end());
  EXPECT_NE(std::find(matches.begin(), matches.end(), "drop"), matches.end());
}

TEST_F(CompletionTest, SubCommandsPrefix) {
  auto matches = sentinel::GetCompletionMatches("s", "/context s", tree);
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], "show");
}

TEST_F(CompletionTest, NoMatch) {
  auto matches = sentinel::GetCompletionMatches("x", "/context x", tree);
  EXPECT_EQ(matches.size(), 0);
}

TEST_F(CompletionTest, DeepNoMatch) {
  auto matches = sentinel::GetCompletionMatches("", "/help ", tree);
  EXPECT_EQ(matches.size(), 0);
}
