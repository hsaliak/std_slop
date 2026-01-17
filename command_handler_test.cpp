#include "command_handler.h"
#include "orchestrator.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace slop {

class CommandHandlerTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http_client;
  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};

TEST_F(CommandHandlerTest, DetectsCommand) {
    CommandHandler handler(&db);
    std::string input = "/help";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, IgnoresNormalText) {
    CommandHandler handler(&db);
    std::string input = "Just some text";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::NOT_A_COMMAND);
}

TEST_F(CommandHandlerTest, HandlesUnknownCommand) {
    CommandHandler handler(&db);
    std::string input = "/unknown_xyz";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}

TEST_F(CommandHandlerTest, HandlesCommandWithWhitespace) {
    CommandHandler handler(&db);
    std::string input = "   /help   ";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, HandlesContextWindow) {
    CommandHandler handler(&db);
    std::string input = "/context window 10";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    
    auto settings = db.GetContextSettings("s1");
    EXPECT_TRUE(settings.ok());
    EXPECT_EQ(settings->size, 10);
}

TEST_F(CommandHandlerTest, DetectsQuitExit) {
    CommandHandler handler(&db);
    std::string input = "/quit";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, ActivatesSkillByName) {
    CommandHandler handler(&db);
    
    Database::Skill skill_obj = {0, "test_skill", "desc", "PATCH"};
    ASSERT_TRUE(db.RegisterSkill(skill_obj).ok());
    
    std::string input = "/skill activate test_skill";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    ASSERT_EQ(active_skills.size(), 1);
    EXPECT_EQ(active_skills[0], "test_skill");
}

TEST_F(CommandHandlerTest, ActivatesSkillByNumericId) {
    CommandHandler handler(&db);
    
    // In a fresh :memory: db, the first skill will have ID 1.
    Database::Skill skill_obj = {0, "first_skill", "desc", "PATCH"};
    ASSERT_TRUE(db.RegisterSkill(skill_obj).ok());
    
    std::string input = "/skill activate 1";
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    auto res = handler.Handle(input, sid, active_skills, [](){});
    
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    ASSERT_EQ(active_skills.size(), 1);
    EXPECT_EQ(active_skills[0], "first_skill");
}

TEST_F(CommandHandlerTest, HandlesThrottle) {
    Orchestrator orchestrator(&db, &http_client);
    CommandHandler handler(&db, &orchestrator);
    
    std::string sid = "s1";
    std::vector<std::string> active_skills;
    
    // Test setting throttle
    std::string input = "/throttle 5";
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    EXPECT_EQ(orchestrator.GetThrottle(), 5);
    
    // Test viewing throttle (just ensures it returns HANDLED)
    input = "/throttle";
    res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);
    EXPECT_EQ(orchestrator.GetThrottle(), 5);
}


TEST_F(CommandHandlerTest, HandlesUndo) {
    CommandHandler handler(&db);
    std::string sid = "s1";
    std::vector<std::string> active_skills;

    // Append two message groups
    ASSERT_TRUE(db.AppendMessage(sid, "user", "msg1", "", "completed", "g1").ok());
    ASSERT_TRUE(db.AppendMessage(sid, "assistant", "resp1", "", "completed", "g1").ok());
    ASSERT_TRUE(db.AppendMessage(sid, "user", "msg2", "", "completed", "g2").ok());
    ASSERT_TRUE(db.AppendMessage(sid, "assistant", "resp2", "", "completed", "g2").ok());

    // Verify both groups exist
    auto history = db.GetConversationHistory(sid);
    ASSERT_TRUE(history.ok());
    EXPECT_EQ(history->size(), 4);

    // Undo last interaction
    std::string input = "/undo";
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);

    // Verify g2 is gone, g1 remains
    history = db.GetConversationHistory(sid);
    ASSERT_TRUE(history.ok());
    EXPECT_EQ(history->size(), 2);
    for (const auto& msg : *history) {
        EXPECT_EQ(msg.group_id, "g1");
    }
}

TEST_F(CommandHandlerTest, HandlesSessionRemove) {
    CommandHandler handler(&db);
    std::string sid = "test_sid";
    std::vector<std::string> active_skills;

    ASSERT_TRUE(db.AppendMessage(sid, "user", "hello").ok());
    ASSERT_TRUE(db.SetContextWindow(sid, 10).ok());

    std::string input = "/session remove test_sid";
    auto res = handler.Handle(input, sid, active_skills, [](){});
    EXPECT_EQ(res, CommandHandler::Result::HANDLED);

    // Verify data is gone
    auto history = db.GetConversationHistory("test_sid");
    EXPECT_EQ(history->size(), 0);

    // Verify session switched if it was the active one
    EXPECT_EQ(sid, "default_session");
}

}  // namespace slop
