#include "command_handler.h"
#include <gtest/gtest.h>

namespace sentinel {

class CommandHandlerTest : public ::testing::Test {
 protected:
  Database db;
  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(CommandHandlerTest, DetectsCommand) {
    CommandHandler handler(&db);
    std::string input = "/help";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, IgnoresNormalText) {
    CommandHandler handler(&db);
    std::string input = "Just some text";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::NOT_A_COMMAND);
}

TEST_F(CommandHandlerTest, HandlesUnknownCommand) {
    CommandHandler handler(&db);
    std::string input = "/unknown_xyz";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}

TEST_F(CommandHandlerTest, HandlesCommandWithWhitespace) {
    CommandHandler handler(&db);
    std::string input = "   /help   ";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, HandlesContextModeFts) {
    CommandHandler handler(&db);
    std::string input = "/context-mode fts 10";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    
    auto settings = db.GetContextSettings("s1");
    EXPECT_TRUE(settings.ok());
    EXPECT_EQ(settings->mode, Database::ContextMode::FTS_RANKED);
    EXPECT_EQ(settings->size, 10);
}

TEST_F(CommandHandlerTest, DetectsQuitExit) {
    CommandHandler handler(&db);
    std::string input = "/quit";
    std::string sid = "s1";
    std::vector<std::string> skills;
    auto res = handler.Handle(input, sid, skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

}  // namespace sentinel
