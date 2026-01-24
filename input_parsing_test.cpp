#include "command_handler.h"
#include "database.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>
namespace slop {

class InputParsingTest : public ::testing::Test {
 protected:
  Database db;
  std::string session_id = "test_session";
  std::vector<std::string> active_skills;

  void SetUp() override {
    auto status = db.Init(":memory:");
    ASSERT_TRUE(status.ok());
  }
};

TEST_F(InputParsingTest, SquareBracesInNormalInput) {
  auto handler_or = CommandHandler::Create(&db, nullptr, nullptr, "", "");
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "This is a test [with square braces]";
  auto result = handler.Handle(input, session_id, active_skills, []() {}, {});

  // Should NOT be handled as a command
  EXPECT_EQ(result, CommandHandler::Result::NOT_A_COMMAND);
}

TEST_F(InputParsingTest, SquareBracesInCommandArgs) {
  auto handler_or = CommandHandler::Create(&db, nullptr, nullptr, "", "");
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  // Many commands just take the rest of the line as args
  std::string input = "/session activate session[1]";
  auto result = handler.Handle(input, session_id, active_skills, []() {}, {});

  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_EQ(session_id, "session[1]");
}

TEST_F(InputParsingTest, SingleQuotesInCommandArgs) {
  auto handler_or = CommandHandler::Create(&db, nullptr, nullptr, "", "");
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  // This tests if the manual SQL construction fails or is vulnerable
  std::string input = "/session activate session' OR '1'='1";
  auto result = handler.Handle(input, session_id, active_skills, []() {}, {});

  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_EQ(session_id, "session' OR '1'='1");
}

TEST_F(InputParsingTest, MalformedCommand) {
  auto handler_or = CommandHandler::Create(&db, nullptr, nullptr, "", "");
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/nonexistent_command [arg]";
  auto result = handler.Handle(input, session_id, active_skills, []() {}, {});

  EXPECT_EQ(result, CommandHandler::Result::UNKNOWN);
}

}  // namespace slop
