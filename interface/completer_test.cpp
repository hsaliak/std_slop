#include "interface/completer.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace slop {

TEST(CompleterTest, FiltersByPrefix) {
  std::vector<std::string> commands = {"/help", "/session", "/skill", "/stats", "/undo"};

  auto result = FilterCommands("/s", commands);
  EXPECT_EQ(result.size(), 3);
  EXPECT_EQ(result[0], "/session");
  EXPECT_EQ(result[1], "/skill");
  EXPECT_EQ(result[2], "/stats");
}

TEST(CompleterTest, EmptyPrefixReturnsAll) {
  std::vector<std::string> commands = {"/help", "/undo"};
  auto result = FilterCommands("", commands);
  EXPECT_EQ(result.size(), 2);
}

TEST(CompleterTest, NoMatchesReturnsEmpty) {
  std::vector<std::string> commands = {"/help", "/undo"};
  auto result = FilterCommands("/x", commands);
  EXPECT_TRUE(result.empty());
}

TEST(CompleterTest, ExactMatch) {
  std::vector<std::string> commands = {"/help", "/undo"};
  auto result = FilterCommands("/help", commands);
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], "/help");
}

}  // namespace slop
