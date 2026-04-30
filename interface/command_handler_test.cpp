#include "interface/command_handler.h"

#include "absl/strings/match.h"

#include "core/orchestrator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
namespace slop {
class TestableCommandHandler : public CommandHandler {
 public:
  explicit TestableCommandHandler(Database* db, class Orchestrator* orchestrator = nullptr,
                                  OAuthHandler* oauth_handler = nullptr, std::string google_api_key = "",
                                  std::string openai_api_key = "")
      : CommandHandler(db, orchestrator, oauth_handler, std::move(google_api_key), std::move(openai_api_key)) {}
  std::string next_editor_output;
  std::string last_initial_content;
  std::string last_extension;
  bool editor_was_called = false;
  absl::flat_hash_map<std::string, absl::StatusOr<std::string>> command_responses;
  std::vector<std::string> executed_commands;
  std::string compact_prompt;
  std::string compact_summary = "LLM compact summary";
  std::vector<std::string> compact_active_skills;
  absl::Status compact_status = absl::OkStatus();

 protected:
  std::string TriggerEditor(const std::string& initial_content, const std::string& extension) override {
    editor_was_called = true;
    last_initial_content = initial_content;
    last_extension = extension;
    return next_editor_output;
  }
  absl::StatusOr<std::string> ExecuteCommand(const std::string& command) override {
    executed_commands.push_back(command);
    if (command_responses.contains(command)) {
      return command_responses.at(command);
    }
    return "";
  }
  absl::StatusOr<std::string> GenerateCompactSummary(const std::string& prompt,
                                                     const std::vector<std::string>& active_skills) override {
    compact_prompt = prompt;
    compact_active_skills = active_skills;
    if (!compact_status.ok()) return compact_status;
    return compact_summary;
  }
};
class CommandHandlerTest : public ::testing::Test {
 protected:
  Database db;
  HttpClient http_client;
  void SetUp() override { ASSERT_TRUE(db.Init(":memory:").ok()); }
};
TEST_F(CommandHandlerTest, DetectsCommand) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/help";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, ReturnsCommandNames) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  auto names = handler.GetCommandNames();
  EXPECT_FALSE(names.empty());
  EXPECT_NE(std::find(names.begin(), names.end(), "/help"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "/session"), names.end());
}
TEST_F(CommandHandlerTest, ReturnsSubCommands) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  auto it = handler.GetSubCommandMap().find("/session");
  ASSERT_NE(it, handler.GetSubCommandMap().end());
  const auto& subs = it->second;
  EXPECT_FALSE(subs.empty());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "list"), subs.end());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "switch"), subs.end());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "clone"), subs.end());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "rollback"), subs.end());
}

TEST_F(CommandHandlerTest, ReturnsScratchpadSubCommands) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  auto it = handler.GetSubCommandMap().find("/scratchpad");
  ASSERT_NE(it, handler.GetSubCommandMap().end());
  const auto& subs = it->second;
  EXPECT_FALSE(subs.empty());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "edit"), subs.end());
  EXPECT_NE(std::find(subs.begin(), subs.end(), "save"), subs.end());
}

TEST_F(CommandHandlerTest, SessionClone) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  // Set up some data in s1
  ASSERT_TRUE(db.AppendMessage(sid, "user", "Hello").ok());
  // Clone s1 to s2
  std::string input = "/session clone s2";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  // Verify sid was updated
  EXPECT_EQ(sid, "s2");
  // Verify s2 has s1's data

  auto history = db.GetConversationHistory("s2");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].content, "Hello");
}

TEST_F(CommandHandlerTest, SessionSwitchThroughGroupCreatesPrefixSessionAndSwitches) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "a1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "a2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u3", "", "completed", "g3").ok());

  std::string input = "/session switch s2 g2";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(sid, "s2");
  auto history = db.GetConversationHistory("s2");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 4);
  EXPECT_EQ((*history)[3].content, "a2");
}

TEST_F(CommandHandlerTest, SessionSwitchCompactAppendsSummaryToExistingSessionAndSwitches) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills = {"planner"};
  ASSERT_TRUE(db.AppendMessage(sid, "user", "hello", "", "completed", "g1").ok());
  handler.compact_summary = "Summarized by the LLM";

  std::string input = "/session switch target compact";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(sid, "target");
  EXPECT_TRUE(absl::StrContains(handler.compact_prompt, "You are compacting a coding-agent conversation"));
  EXPECT_TRUE(absl::StrContains(handler.compact_prompt, "Output only the compacted summary in Markdown"));
  EXPECT_TRUE(absl::StrContains(handler.compact_prompt, "[user] (group: g1)"));
  EXPECT_TRUE(absl::StrContains(handler.compact_prompt, "hello"));
  ASSERT_EQ(handler.compact_active_skills.size(), 1);
  EXPECT_EQ(handler.compact_active_skills[0], "planner");
  auto history = db.GetConversationHistory("target");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].role, "assistant");
  EXPECT_EQ((*history)[0].group_id, "compact");
  EXPECT_EQ((*history)[0].content, "Summarized by the LLM");
}

TEST_F(CommandHandlerTest, SessionRollbackDropsLaterMessages) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "a1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "a2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "user", "u3", "", "completed", "g3").ok());

  std::string input = "/session rollback g2";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(sid, "s1");
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 4);
  EXPECT_EQ((*history)[3].content, "a2");
}

TEST_F(CommandHandlerTest, IgnoresNormalText) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "Just some text";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::NOT_A_COMMAND);
}
TEST_F(CommandHandlerTest, HandlesUnknownCommand) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/unknown_xyz";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}
TEST_F(CommandHandlerTest, HandlesCommandWithWhitespace) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "   /help   ";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, HandlesContextWindow) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/context";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, ContextShowIsHandled) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/context show";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, WindowAliasIsRemoved) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/window 10";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::UNKNOWN);
}
TEST_F(CommandHandlerTest, DetectsQuitExit) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string input = "/quit";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, ActivatesSkillByName) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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
  auto orchestrator_or = Orchestrator::Builder(&db, &http_client).Build();
  ASSERT_TRUE(orchestrator_or.ok());
  auto& orchestrator = *orchestrator_or;
  auto handler_or = CommandHandler::Create(&db, orchestrator.get());
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  // Test setting throttle
  std::string input = "/throttle 5";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_EQ(orchestrator->GetThrottle(), 5);
}
TEST_F(CommandHandlerTest, MessageListHandlesNullTokens) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "test_session";
  std::vector<std::string> active_skills;
  // Add a user message without a corresponding assistant message
  // This simulates the case where MAX(tokens) returns NULL
  ASSERT_TRUE(db.AppendMessage(sid, "user", "user message without assistant", "", "completed", "g1", "", 0).ok());
  // This should not crash even though tokens will be NULL in the JOIN
  std::string input = "/message list";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, MessageListWithMixedTokens) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "test_session";
  std::vector<std::string> active_skills;
  // Add user+assistant pair with tokens
  ASSERT_TRUE(db.AppendMessage(sid, "user", "msg with response", "", "completed", "g1", "", 0).ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "assistant response", "", "completed", "g1", "", 100).ok());
  // Add user message without assistant (tokens will be NULL)
  ASSERT_TRUE(db.AppendMessage(sid, "user", "msg without response", "", "completed", "g2", "", 0).ok());
  // This should handle both cases without crashing
  std::string input = "/message list";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
}
TEST_F(CommandHandlerTest, HandlesUndo) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
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

TEST_F(CommandHandlerTest, ScratchpadEditUsesExistingContent) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  ASSERT_TRUE(db.SetScratchpad(sid, "initial plan").ok());

  handler.next_editor_output = "edited plan";
  std::string input = "/scratchpad edit";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_EQ(handler.last_initial_content, "initial plan");

  auto content_or = db.GetScratchpad(sid);
  ASSERT_TRUE(content_or.ok());
  EXPECT_EQ(*content_or, "edited plan");
}

TEST_F(CommandHandlerTest, ScratchpadSaveStoresLastAssistantMessage) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "latest answer").ok());

  std::string input = "/scratchpad save";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);

  auto content_or = db.GetScratchpad(sid);
  ASSERT_TRUE(content_or.ok());
  EXPECT_EQ(*content_or, "latest answer");
}

TEST_F(CommandHandlerTest, SkillEditUsingEditor) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  // Register a skill
  Database::Skill s{0, "myskill", "A test skill", "ORIGINAL PATCH"};
  ASSERT_TRUE(db.RegisterSkill(s).ok());
  // Set up mock editor
  // We return a modified Markdown.
  handler.next_editor_output = "# Name: myskill\n# Description: A test skill\n\n# System Prompt Patch\nEDITED PATCH";
  handler.editor_was_called = false;
  // Edit it
  std::string input = "/skill edit myskill";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_TRUE(absl::StrContains(handler.last_initial_content, "ORIGINAL PATCH"));
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
TEST_F(CommandHandlerTest, SkillEditEmptyDeletes) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  Database::Skill s{0, "deleteme", "desc", "patch"};
  ASSERT_TRUE(db.RegisterSkill(s).ok());
  handler.next_editor_output = "   ";  // Empty/whitespace
  std::string input = "/skill edit deleteme";
  handler.Handle(input, sid, active_skills, []() {}, {});
  auto skills = db.GetSkills();
  for (const auto& sk : *skills) {
    EXPECT_NE(sk.name, "deleteme");
  }
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
TEST_F(CommandHandlerTest, ReviewFailsOutsideGit) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "fatal: not a git repository";
  std::string input = "/review";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_FALSE(handler.editor_was_called);
}
TEST_F(CommandHandlerTest, ReviewHandlesChanges) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git ls-files --others --exclude-standard"] = "new_file.cpp";
  handler.command_responses["git add -N -- 'new_file.cpp'"] = "";
  handler.command_responses["git diff"] = "diff --git a/old.cpp b/old.cpp\n+new line";
  handler.next_editor_output = "diff --git a/old.cpp b/old.cpp\n+new line\nR: This looks good";
  std::string input = "/review session";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_TRUE(absl::StrContains(input, "R: This looks good"));
  // Verify git add -N was called for the new file with quoting
  bool found_add = false;
  for (const auto& cmd : handler.executed_commands) {
    if (cmd == "git add -N -- 'new_file.cpp'") found_add = true;
  }
  EXPECT_TRUE(found_add);
}
TEST_F(CommandHandlerTest, ReviewRequiresPrefixAtStartOfLine) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git diff"] = "some diff";
  // R: in the middle of a line should not trigger
  handler.next_editor_output = "This line has R: but not at start";
  std::string input = "/review session";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  // R: at start of line should trigger
  handler.next_editor_output = "R: This is a comment";
  res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
}
TEST_F(CommandHandlerTest, ReviewHistorical) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git diff HEAD~1"] = "diff --git a/old.cpp b/old.cpp\n+historical line";
  handler.next_editor_output = "R: Reviewing history";
  std::string input = "/review git 1";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  // Verify git diff HEAD~1 was called
  bool found_diff = false;
  bool found_ls_files = false;
  for (const auto& cmd : handler.executed_commands) {
    if (cmd == "git diff HEAD~1") found_diff = true;
    if (absl::StrContains(cmd, "ls-files")) found_ls_files = true;
  }
  EXPECT_TRUE(found_diff);
  EXPECT_FALSE(found_ls_files);  // Should skip untracked files for historical diff
}
TEST_F(CommandHandlerTest, ReviewRef) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git diff main"] = "diff --git a/old.cpp b/old.cpp\n+ref line";
  handler.next_editor_output = "R: Reviewing ref";
  std::string input = "/review git main";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  // Verify git diff main was called
  bool found_diff = false;
  for (const auto& cmd : handler.executed_commands) {
    if (cmd == "git diff main") found_diff = true;
  }
  EXPECT_TRUE(found_diff);
}
TEST_F(CommandHandlerTest, FeedbackFailsWithNoHistory) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  std::string input = "/feedback";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_FALSE(handler.editor_was_called);
}
TEST_F(CommandHandlerTest, FeedbackFailsWithNoAssistantMessage) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  ASSERT_TRUE(db.AppendMessage(sid, "user", "hello").ok());
  std::vector<std::string> active_skills;
  std::string input = "/feedback";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_FALSE(handler.editor_was_called);
}
TEST_F(CommandHandlerTest, FeedbackHandlesAssistantMessage) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  ASSERT_TRUE(db.AppendMessage(sid, "user", "hello").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "I am an assistant", "", "completed", "", "openai").ok());
  std::vector<std::string> active_skills;
  std::string input = "/feedback";
  handler.next_editor_output = "1: I am an assistant\nR: Great job";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_TRUE(absl::StrContains(input, "R: Great job"));
  EXPECT_TRUE(absl::StrContains(handler.last_initial_content, "1: I am an assistant"));
}
TEST_F(CommandHandlerTest, FeedbackNoCommentsDoesNothing) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  ASSERT_TRUE(db.AppendMessage(sid, "assistant", "I am an assistant", "", "completed", "", "openai").ok());
  std::vector<std::string> active_skills;
  std::string input = "/feedback";
  handler.next_editor_output = "1: I am an assistant\nNo comments here";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(handler.editor_was_called);
}
TEST_F(CommandHandlerTest, ToolListDoesNotShowPromotedJsToolsSecondTable) {
  auto handler_or = CommandHandler::Create(&db);
  ASSERT_TRUE(handler_or.ok());
  auto& handler = **handler_or;

  std::string sid = "s1";
  std::vector<std::string> active_skills;

  testing::internal::CaptureStdout();
  std::string input = "/tool list";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "Available Tools"));
  EXPECT_FALSE(absl::StrContains(output, "Promoted JS Tools (Top-Level Enabled)"));
}

TEST_F(CommandHandlerTest, SkillListTruncatesDescription) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  std::string long_desc =
      "This is a very long description that should definitely be truncated because it exceeds sixty characters in "
      "length.";
  Database::Skill s{0, "long_skill", long_desc, "patch"};
  ASSERT_TRUE(db.RegisterSkill(s).ok());
  testing::internal::CaptureStdout();
  std::string input = "/skill list";
  handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  // Check for truncated description in output
  // We check for chunks that are likely to stay together and not be broken by ANSI or wrapping too much.
  EXPECT_TRUE(absl::StrContains(output, "This is a very long"));
  EXPECT_TRUE(absl::StrContains(output, "definitely be..."));
  EXPECT_FALSE(output.find(long_desc) != std::string::npos);
}
TEST_F(CommandHandlerTest, SkillListCleansNewlinesAndPipes) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  std::string messy_desc = "Line 1\nLine 2 | Pipe";
  Database::Skill s{0, "messy_skill", messy_desc, "patch"};
  ASSERT_TRUE(db.RegisterSkill(s).ok());
  testing::internal::CaptureStdout();
  std::string input = "/skill list";
  handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  // Check for cleaned description
  EXPECT_TRUE(absl::StrContains(output, "Line 1 Line 2"));
  EXPECT_TRUE(absl::StrContains(output, "\\| Pipe"));
}
TEST_F(CommandHandlerTest, ModeMailRequiresGit) {
  TestableCommandHandler handler(&db);
  std::string session_id = "test_session";
  std::vector<std::string> active_skills;
  std::string input = "/mode mail";
  // 1. Test failure when not in a git repo
  handler.command_responses["git rev-parse --is-inside-work-tree"] = absl::NotFoundError("not a git repo");
  testing::internal::CaptureStdout();
  auto result = handler.Handle(input, session_id, active_skills, []() {});
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "Error: Not a git repository. Please run 'git init' first."));
  EXPECT_FALSE(absl::StrContains(output, "Switched to MAIL mode"));
  // 2. Test success when in a git repo
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  testing::internal::CaptureStdout();
  result = handler.Handle(input, session_id, active_skills, []() {});
  output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "Switched to MAIL mode"));
}
TEST_F(CommandHandlerTest, ModeMailRequiresCleanRepo) {
  TestableCommandHandler handler(&db);
  std::string session_id = "test_session";
  std::vector<std::string> active_skills;
  std::string input = "/mode mail";
  // 1. Repo is inside work tree but dirty
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git status --porcelain"] = "M interface/command_handler.cpp";
  testing::internal::CaptureStdout();
  auto result = handler.Handle(input, session_id, active_skills, []() {});
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "Error: Git repository is dirty"));
  EXPECT_TRUE(absl::StrContains(output, "Dirty files:"));
  EXPECT_TRUE(absl::StrContains(output, "M interface/command_handler.cpp"));
  EXPECT_FALSE(absl::StrContains(output, "Switched to MAIL mode"));
  // 2. Repo is inside work tree and clean
  handler.command_responses["git status --porcelain"] = "";
  testing::internal::CaptureStdout();
  result = handler.Handle(input, session_id, active_skills, []() {});
  output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "Switched to MAIL mode"));
}

TEST_F(CommandHandlerTest, RefreshMailModeFromDbTracksSettingsMode) {
  TestableCommandHandler handler(&db);
  EXPECT_FALSE(handler.IsMailMode());

  ASSERT_TRUE(db.Query("UPDATE settings SET mode = 'mail' WHERE id = 1").ok());
  handler.RefreshMailModeFromDb();
  EXPECT_TRUE(handler.IsMailMode());

  ASSERT_TRUE(db.Query("UPDATE settings SET mode = 'standard' WHERE id = 1").ok());
  handler.RefreshMailModeFromDb();
  EXPECT_FALSE(handler.IsMailMode());
}

TEST_F(CommandHandlerTest, ReviewPatchUsesPatchExtension) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  // Mock git commands for GitFormatPatchSeries
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git rev-list --reverse main..HEAD"] = "hash1";
  handler.command_responses["git show --no-patch --format='%s%n%nRationale: %b' hash1"] = "feat: test patch";
  handler.command_responses["git show hash1"] = "diff content";
  handler.next_editor_output = "R: LGTM";
  std::string input = "/review mail";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  EXPECT_TRUE(handler.editor_was_called);
  EXPECT_EQ(handler.last_extension, ".patch");
  EXPECT_TRUE(absl::StrContains(handler.last_initial_content, "--- MAIL REVIEW ---"));
  EXPECT_FALSE(absl::StrContains(handler.last_initial_content, "Add your comments starting with 'R:' below."));
}
TEST_F(CommandHandlerTest, ReviewPatchDiagnosticsOnBaseBranch) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  // 1. Mock we are on main and comparing main..HEAD (empty)
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "main";

  handler.command_responses["git rev-list --reverse main..HEAD"] = "";  // No patches
  testing::internal::CaptureStdout();
  std::string input = "/review mail";
  handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_TRUE(absl::StrContains(output, "No patches found to review in range main..HEAD"));
  EXPECT_TRUE(absl::StrContains(output, "Tip: You are currently on the base branch 'main'"));
}
TEST_F(CommandHandlerTest, ReviewMailEmptyStagingBranch) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "slop/staging/feature";

  handler.command_responses["git rev-list --reverse main..HEAD"] = "";
  testing::internal::CaptureStdout();
  std::string input = "/review mail";
  handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_TRUE(absl::StrContains(output, "No patches found to review in range main..HEAD"));
  EXPECT_TRUE(absl::StrContains(output,
                                "Tip: If you are on a staging branch, in mail mode, but do not see patches yet, ask "
                                "the agent to git_commit_patch the changes as a patch"));
}
TEST_F(CommandHandlerTest, ReviewStandardSuggestsMail) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git diff HEAD"] = "";  // No changes
  // 1. Without mail mode
  {
    testing::internal::CaptureStdout();
    std::string input = "/review session";
    handler.Handle(input, sid, active_skills, []() {}, {});
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(absl::StrContains(output, "No changes to review."));
    EXPECT_FALSE(absl::StrContains(output, "Tip: Did you mean to use '/review mail'"));
  }
  // 2. With mail mode
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";  // reset/ensure git
  // Switch to mail mode first
  {
    std::string mode_input = "/mode mail";
    handler.Handle(mode_input, sid, active_skills, []() {});
  }
  {
    testing::internal::CaptureStdout();
    std::string input = "/review session";
    handler.Handle(input, sid, active_skills, []() {}, {});
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(absl::StrContains(output, "No changes to review."));
    EXPECT_TRUE(
        absl::StrContains(output, "Tip: Did you mean to use '/review mail' to review patches in the current series?"));
  }
}
TEST_F(CommandHandlerTest, ReviewMailApproveProceedsToLLM) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "slop/staging/feature";

  handler.command_responses["git rev-list --reverse main..HEAD"] = "commit_hash_123";
  handler.command_responses["git rev-parse HEAD"] = "abcd123";
  std::string input = "/review mail approve";
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  EXPECT_EQ(res, CommandHandler::Result::PROCEED_TO_LLM);
  EXPECT_TRUE(
      absl::StrContains(input, "I have approved the patchset for branch 'slop/staging/feature' at hash abcd123"));
  // Verify database record
  auto approved_hash = db.GetPatchApproval("slop/staging/feature");
  ASSERT_TRUE(approved_hash.ok());
  EXPECT_EQ(*approved_hash, "abcd123");
}

TEST_F(CommandHandlerTest, ReviewMailDetachedHeadShowsRecoveryTip) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "HEAD";

  std::string input = "/review mail";
  testing::internal::CaptureStderr();
  testing::internal::CaptureStdout();
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(err, "detached HEAD"));
  EXPECT_TRUE(absl::StrContains(output, "/mode mail <name>"));
}

TEST_F(CommandHandlerTest, ReviewDashboard) {
  TestableCommandHandler handler(&db);
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git status --porcelain"] = "M file.cpp";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "slop/staging/feature";

  handler.command_responses["git rev-list --count main..HEAD"] = "1";
  std::string input = "/review";
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  testing::internal::CaptureStdout();
  auto res = handler.Handle(input, sid, active_skills, []() {}, {});
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(res, CommandHandler::Result::HANDLED);
  EXPECT_TRUE(absl::StrContains(output, "--- Review Dashboard ---"));
  EXPECT_TRUE(absl::StrContains(output, "/review session"));
  EXPECT_TRUE(absl::StrContains(output, "/review mail"));
  EXPECT_TRUE(absl::StrContains(output, "/review git <ref>"));
}
TEST_F(CommandHandlerTest, ModeMailResolvesCorrectBaseBranch) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  // Case 1: On 'develop' (non-staging)
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "develop";
  {
    testing::internal::CaptureStdout();
    std::string input = "/mode mail";
    handler.Handle(input, sid, active_skills, []() {});
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(absl::StrContains(output, "Base Branch: develop"));
  }
  // Case 2: On 'slop/staging/fix' (staging)
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "slop/staging/fix";
  (void)db.Execute("INSERT INTO staging_branches (branch_name, parent_branch) VALUES (?, ?);", "slop/staging/fix",
                   "main");
  {
    testing::internal::CaptureStdout();
    std::string input = "/mode mail";
    handler.Handle(input, sid, active_skills, []() {});
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(absl::StrContains(output, "Base Branch: main"));
  }
}

TEST_F(CommandHandlerTest, ModeMailWithBranchNameCreatesStagingAndSetsBase) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git status --porcelain"] = "";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "develop";
  handler.command_responses["git checkout -b slop/staging/feature123 develop"] = "";

  testing::internal::CaptureStdout();
  std::string input = "/mode mail feature123";
  handler.Handle(input, sid, active_skills, []() {});
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_TRUE(absl::StrContains(output, "Created staging branch: slop/staging/feature123"));
  EXPECT_TRUE(absl::StrContains(output, "Branch: slop/staging/feature123"));
  EXPECT_TRUE(absl::StrContains(output, "Base Branch: develop"));

  auto rows_or = db.Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?;", {"slop/staging/feature123"});
  ASSERT_TRUE(rows_or.ok());
  EXPECT_TRUE(absl::StrContains(*rows_or, "develop"));
}

TEST_F(CommandHandlerTest, ModeMailWithBranchNameFallsBackToExistingStagingBranch) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;
  handler.command_responses["git rev-parse --is-inside-work-tree"] = "true";
  handler.command_responses["git status --porcelain"] = "";
  handler.command_responses["git rev-parse --abbrev-ref HEAD"] = "main";
  handler.command_responses["git checkout -b slop/staging/featureabc main"] = absl::InternalError("already exists");
  handler.command_responses["git checkout slop/staging/featureabc"] = "";

  testing::internal::CaptureStdout();
  std::string input = "/mode mail featureabc";
  handler.Handle(input, sid, active_skills, []() {});
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_TRUE(absl::StrContains(output, "Switched to existing staging branch: slop/staging/featureabc"));
  EXPECT_TRUE(absl::StrContains(output, "Branch: slop/staging/featureabc"));
  EXPECT_TRUE(absl::StrContains(output, "Base Branch: main"));
}

TEST_F(CommandHandlerTest, ModeMailWithInvalidBranchNameIsRejected) {
  TestableCommandHandler handler(&db);
  std::string sid = "s1";
  std::vector<std::string> active_skills;

  testing::internal::CaptureStdout();
  std::string input = "/mode mail feature/name";
  handler.Handle(input, sid, active_skills, []() {});
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_TRUE(absl::StrContains(output, "Error: Invalid branch name"));
}
}  // namespace slop
