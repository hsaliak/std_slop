#include "command_handler.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "orchestrator.h"
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
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, IgnoresNormalText) {
  CommandHandler handler(&db);
  std::string input = "Just some text";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::NOT_A_COMMAND);
}

TEST_F(CommandHandlerTest, HandlesUnknownCommand) {
  CommandHandler handler(&db);
  std::string input = "/unknown_xyz";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}

TEST_F(CommandHandlerTest, HandlesCommandWithWhitespace) {
  CommandHandler handler(&db);
  std::string input = "   /help   ";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, HandlesContextWindow) {
  CommandHandler handler(&db);
  std::string input = "/context window 10";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);

  auto settings = db.GetContextSettings("s1");
  EXPECT_TRUE(settings.ok());
  EXPECT_EQ(settings->size, 10);
}

TEST_F(CommandHandlerTest, ContextWithoutSubcommandShowsUsage) {
  CommandHandler handler(&db);
  std::string input = "/context";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, ContextShowIsHandled) {
  CommandHandler handler(&db);
  std::string input = "/context show";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, WindowAliasIsRemoved) {
  CommandHandler handler(&db);
  std::string input = "/window 10";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}

TEST_F(CommandHandlerTest, DetectsQuitExit) {
  CommandHandler handler(&db);
  std::string input = "/quit";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}

TEST_F(CommandHandlerTest, ActivatesSkillByName) {
  CommandHandler handler(&db);

  Database::Skill skill_obj = {0, "test_skill", "desc", "PATCH"};
  ASSERT_TRUE(db.RegisterSkill(skill_obj).ok());

  std::string input = "/skill activate test_skill";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  ASSERT_EQ(active_skills.size(), 1);
  EXPECT_EQ(active_skills[0], "test_skill");
}

TEST_F(CommandHandlerTest, ActivatesSkillByNumericId) {
  CommandHandler handler(&db);

  Database::Skill skill_obj = {0, "extra_skill", "desc", "PATCH"};
  ASSERT_TRUE(db.RegisterSkill(skill_obj).ok());

  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  int target_id = -1;
  for (const auto& s : *skills) {
    if (s.name == "extra_skill") target_id = s.id;
  }
  ASSERT_NE(target_id, -1);

  std::string input = "/skill activate " + std::to_string(target_id);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  ASSERT_EQ(active_skills.size(), 1);
  EXPECT_EQ(active_skills[0], "extra_skill");
}

TEST_F(CommandHandlerTest, DeactivatesSkill) {
  CommandHandler handler(&db);
  std::string sid = "s1";

  Database::Skill skill1 = {0, "skill1", "desc", "PATCH"};
  Database::Skill skill2 = {0, "skill2", "desc", "PATCH"};
  ASSERT_TRUE(db.RegisterSkill(skill1).ok());
  ASSERT_TRUE(db.RegisterSkill(skill2).ok());

  std::vector<std::string> active_skills = {"skill1", "skill2"};

  std::string input = "/skill deactivate skill1";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  ASSERT_EQ(active_skills.size(), 1);
  EXPECT_EQ(active_skills[0], "skill2");
}

TEST_F(CommandHandlerTest, HandlesThrottle) {
  auto orchestrator = Orchestrator::Builder(&db, &http_client).Build();
  CommandHandler handler(&db, orchestrator.get());

  std::string sid = "s1";
  std::vector<std::string> active_skills;

  // Test setting throttle
  std::string input = "/throttle 5";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(orchestrator->GetThrottle(), 5);
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
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);

  // Verify g2 is gone, g1 remains
  history = db.GetConversationHistory(sid);
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), 2);
}

TEST_F(CommandHandlerTest, HandlesSessionRemove) {
  CommandHandler handler(&db);
  std::string sid = "test_sid";
  std::vector<std::string> active_skills;

  ASSERT_TRUE(db.AppendMessage(sid, "user", "hello").ok());
  ASSERT_TRUE(db.SetContextWindow(sid, 10).ok());

  std::string input = "/session remove test_sid";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(sid, "default_session");

  auto history = db.GetConversationHistory("test_sid");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), 0);
}

TEST_F(CommandHandlerTest, HandlesTodoCommands) {
  CommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;

  // /todo add
  std::string input = "/todo add mygroup Do the thing";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);

  auto todos = db.GetTodos("mygroup");
  ASSERT_TRUE(todos.ok());
  ASSERT_EQ(todos->size(), 1);
  EXPECT_EQ((*todos)[0].description, "Do the thing");

  // /todo edit
  input = "/todo edit mygroup 1 New description";
  res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  todos = db.GetTodos("mygroup");
  EXPECT_EQ((*todos)[0].description, "New description");

  // /todo complete
  input = "/todo complete mygroup 1";
  res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  todos = db.GetTodos("mygroup");
  EXPECT_EQ((*todos)[0].status, "Complete");

  // /todo drop
  input = "/todo drop mygroup";
  res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  todos = db.GetTodos("mygroup");
  EXPECT_EQ(todos->size(), 0);
}

}  // namespace slop
