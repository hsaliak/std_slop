#include "command_handler.h"
#include "orchestrator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
namespace slop {

class TestableCommandHandler : public CommandHandler {
 public:
  using CommandHandler::CommandHandler;

  std::string next_editor_output;
  std::string last_initial_content;
  bool editor_was_called = false;

 protected:
  std::string TriggerEditor(const std::string& initial_content) override {
    editor_was_called = true;
    last_initial_content = initial_content;
    return next_editor_output;
  }
};

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

TEST_F(CommandHandlerTest, ReturnsCommandNames) {
  CommandHandler handler(&db);
  auto names = handler.GetCommandNames();
  EXPECT_FALSE(names.empty());
  EXPECT_NE(std::find(names.begin(), names.end(), "/help"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "/session"), names.end());
}

TEST_F(CommandHandlerTest, ReturnsSubCommands) {
  CommandHandler handler(&db);
  auto subs = handler.GetSubCommands("/session");
  EXPECT_FALSE(subs.empty());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "list"), subs.end());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "activate"), subs.end());
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

TEST_F(CommandHandlerTest, SessionScratchpadEditSaves) {
  TestableCommandHandler handler(&db);
  std::string sid = "test_scratch_session";
  std::vector<std::string> active_skills;

  handler.next_editor_output = "New scratchpad content";
  std::string input = "/session scratchpad edit";

  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(handler.editor_was_called);

  auto saved = db.GetScratchpad(sid);
  ASSERT_TRUE(saved.ok()) << saved.status().message();
  EXPECT_EQ(*saved, "New scratchpad content");
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

TEST_F(CommandHandlerTest, SkillEditUsingEditor) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;

  // Register a skill
  Database::Skill s{0, "myskill", "A test skill", "ORIGINAL PATCH"};
  ASSERT_TRUE(db.RegisterSkill(s).ok());

  // Set up mock editor
  // We expect the editor to receive a JSON dump.
  // We return a modified JSON.
  nlohmann::json edited_json;
  edited_json["name"] = "myskill";
  edited_json["description"] = "A test skill";
  edited_json["system_prompt_patch"] = "EDITED PATCH";

  handler.next_editor_output = edited_json.dump(2);
  handler.editor_was_called = false;

  // Edit it
  std::string input = "/skill edit myskill";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(handler.editor_was_called);
  // We don't strictly assert exact JSON format of last_initial_content as it might vary,
  // but we can check if it contains the original patch.
  EXPECT_TRUE(handler.last_initial_content.find("ORIGINAL PATCH") != std::string::npos);

  // Verify update
  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  bool found = false;
  for (const auto& sk : *skills) {
    if (sk.name == "myskill") {
      EXPECT_EQ(sk.system_prompt_patch, "EDITED PATCH");
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(CommandHandlerTest, EditCommandUsingEditor) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;

  handler.next_editor_output = "New input from editor";
  handler.editor_was_called = false;

  std::string input = "/edit";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_EQ(input, "New input from editor");
}
}  // namespace slop
