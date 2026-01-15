#include <gtest/gtest.h>
#include "completion.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

using json = nlohmann::json;

namespace slop {
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
  auto matches = slop::GetCompletionMatches("/", "/", tree);
  EXPECT_EQ(matches.size(), 3);
  EXPECT_NE(std::find(matches.begin(), matches.end(), "/context"), matches.end());
  EXPECT_NE(std::find(matches.begin(), matches.end(), "/help"), matches.end());
}

TEST_F(CompletionTest, RootCommandsPrefix) {
  auto matches = slop::GetCompletionMatches("/c", "/c", tree);
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], "/context");
}

TEST_F(CompletionTest, SubCommands) {
  auto matches = slop::GetCompletionMatches("", "/context ", tree);
  EXPECT_EQ(matches.size(), 2);
  EXPECT_NE(std::find(matches.begin(), matches.end(), "show"), matches.end());
  EXPECT_NE(std::find(matches.begin(), matches.end(), "drop"), matches.end());
}

TEST_F(CompletionTest, SubCommandsPrefix) {
  auto matches = slop::GetCompletionMatches("s", "/context s", tree);
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], "show");
}

TEST_F(CompletionTest, DeepSubCommands) {
  json deep_tree = {
    {"/a", {
      {"b", {
        {"c", {}}
      }}
    }}
  };
  auto matches = slop::GetCompletionMatches("", "/a b ", deep_tree);
  EXPECT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0], "c");
}

TEST_F(CompletionTest, MultipleTrailingSpaces) {
  auto matches = slop::GetCompletionMatches("", "/context  ", tree);
  // Current logic might be sensitive to multiple spaces
  // Tokenize uses ss >> token which skips all whitespace
  // depth = tokens.size() if has_trailing_space
  // For "/context  ", tokens = {"/context"}, has_trailing_space = true, depth = 1
  // current_node will be tree["/context"], which contains "show", "drop"
  EXPECT_EQ(matches.size(), 2);
}

TEST_F(CompletionTest, NoMatch) {
  auto matches = slop::GetCompletionMatches("x", "/context x", tree);
  EXPECT_EQ(matches.size(), 0);
}

TEST_F(CompletionTest, DeepNoMatch) {
  auto matches = slop::GetCompletionMatches("", "/help ", tree);
  EXPECT_EQ(matches.size(), 0);
}
