#include "core/database.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/strings/match.h"

#include "json_utils.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sqlite3.h>

TEST(DatabaseTest, InitWorks) {
  slop::Database db;
  auto status = db.Init(":memory:");
  EXPECT_TRUE(status.ok()) << status.message();
}
TEST(DatabaseTest, InitDropsLegacySessionStateTable) {
  const std::string path = absl::StrCat(::testing::TempDir(), "/std_slop_session_state_",
                                        absl::ToUnixNanos(absl::Now()), ".db");
  sqlite3* raw_db = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw_db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_db, "CREATE TABLE session_state (session_id TEXT PRIMARY KEY, state_blob TEXT);", nullptr,
                         nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_db, "INSERT INTO session_state VALUES ('s1', 'legacy state');", nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_db), SQLITE_OK);

  {
    slop::Database db;
    ASSERT_TRUE(db.Init(path).ok());
    auto tables_or = db.Query("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'session_state'");
    ASSERT_TRUE(tables_or.ok());
    auto tables = slop::json_parse(*tables_or);
    ASSERT_TRUE(tables.has_value());
    EXPECT_TRUE(tables->empty());
    EXPECT_TRUE(db.AppendMessage("s1", "user", "still usable").ok());
  }
  EXPECT_EQ(std::remove(path.c_str()), 0);
}
TEST(DatabaseTest, TablesExist) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Check if tables exist by trying to insert/select
  EXPECT_TRUE(db.Execute("INSERT INTO tools (name, description) VALUES ('test_tool', 'a test tool')").ok());
  EXPECT_TRUE(db.Execute("INSERT INTO messages (session_id, role, content) VALUES ('session1', 'user', 'hello')").ok());
}
TEST(DatabaseTest, DefaultSkillsAndToolsRegistered) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  // We expect at least the 4 default skills we added
  EXPECT_GE(skills->size(), 4);

  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  bool found_run_js = false;
  bool found_query_db = false;
  bool found_read_file = false;
  bool read_file_is_run_js_callable = false;
  bool found_execute_bash = false;
  bool execute_bash_is_run_js_callable = false;
  bool found_edit_tool = false;
  bool edit_tool_is_run_js_callable = false;
  bool found_write_file = false;
  bool write_file_is_run_js_callable = false;
  bool found_persist_function = false;
  bool persist_function_is_run_js_callable = false;
  bool found_ask_user = false;
  for (const auto& t : *tools) {
    if (t.name == "run_js") found_run_js = true;
    if (t.name == "query_db") found_query_db = true;
    if (t.name == "read_file") {
      found_read_file = true;
      read_file_is_run_js_callable = t.is_run_js_callable;
    }
    if (t.name == "execute_bash") {
      found_execute_bash = true;
      execute_bash_is_run_js_callable = t.is_run_js_callable;
    }
    if (t.name == "edit_tool") {
      found_edit_tool = true;
      edit_tool_is_run_js_callable = t.is_run_js_callable;
    }
    if (t.name == "write_file") {
      found_write_file = true;
      write_file_is_run_js_callable = t.is_run_js_callable;
    }
    if (t.name == "persist_function") {
      found_persist_function = true;
      persist_function_is_run_js_callable = t.is_run_js_callable;
    }
    if (t.name == "ask_user") {
      found_ask_user = true;
      EXPECT_FALSE(t.is_run_js_callable);
    }
  }
  EXPECT_TRUE(found_run_js);
  EXPECT_TRUE(found_query_db);
  EXPECT_TRUE(found_read_file);
  EXPECT_TRUE(read_file_is_run_js_callable);
  EXPECT_TRUE(found_execute_bash);
  EXPECT_TRUE(execute_bash_is_run_js_callable);
  EXPECT_TRUE(found_edit_tool);
  EXPECT_TRUE(edit_tool_is_run_js_callable);
  EXPECT_TRUE(found_write_file);
  EXPECT_TRUE(write_file_is_run_js_callable);
  EXPECT_TRUE(found_persist_function);
  EXPECT_TRUE(persist_function_is_run_js_callable);
  EXPECT_TRUE(found_ask_user);

  auto top_level_tools = db.GetTopLevelTools();
  ASSERT_TRUE(top_level_tools.ok());
  auto is_top_level = [&](const std::string& name) {
    return std::any_of(top_level_tools->begin(), top_level_tools->end(),
                       [&](const auto& t) { return t.name == name; });
  };
  EXPECT_TRUE(is_top_level("query_db"));
  EXPECT_TRUE(is_top_level("run_js"));
  EXPECT_TRUE(is_top_level("ask_user"));
  EXPECT_TRUE(is_top_level("read_file"));
  EXPECT_TRUE(is_top_level("list_directory"));
  EXPECT_TRUE(is_top_level("grep"));
  EXPECT_TRUE(is_top_level("execute_bash"));
  EXPECT_TRUE(is_top_level("edit_tool"));
  EXPECT_TRUE(is_top_level("write_file"));
  EXPECT_TRUE(is_top_level("git_commit_patch"));
  EXPECT_TRUE(is_top_level("git_finalize_series"));
  EXPECT_FALSE(is_top_level("persist_function"));
  EXPECT_TRUE(*db.IsRunJsCallableTool("read_file"));
  EXPECT_TRUE(*db.IsRunJsCallableTool("execute_bash"));
  EXPECT_TRUE(*db.IsRunJsCallableTool("edit_tool"));
  EXPECT_TRUE(*db.IsRunJsCallableTool("write_file"));
  EXPECT_TRUE(*db.IsRunJsCallableTool("persist_function"));
  EXPECT_FALSE(*db.IsRunJsCallableTool("ask_user"));
  EXPECT_FALSE(*db.IsRunJsCallableTool("git_finalize_series"));
  EXPECT_FALSE(*db.IsRunJsCallableTool("git_commit_patch"));

  auto js_functions_res = db.Query("SELECT name FROM js_functions");
  ASSERT_TRUE(js_functions_res.ok()) << js_functions_res.status();
  bool found_planner = false;
  bool found_code_reviewer = false;
  bool found_patcher = false;
  bool found_self_improvement_learner = false;
  bool found_subagent_creator = false;
  bool found_dynamic_workflow_harness = false;
  std::string patcher_prompt;
  std::string self_improvement_learner_prompt;
  std::string subagent_creator_prompt;
  std::string dynamic_workflow_harness_prompt;
  for (const auto& s : *skills) {
    if (s.name == "planner") found_planner = true;
    if (s.name == "code_reviewer") found_code_reviewer = true;
    if (s.name == "patcher") {
      found_patcher = true;
      patcher_prompt = s.system_prompt_patch;
    }
    if (s.name == "self_improvement_learner") {
      found_self_improvement_learner = true;
      self_improvement_learner_prompt = s.system_prompt_patch;
    }
    if (s.name == "subagent_creator") {
      found_subagent_creator = true;
      subagent_creator_prompt = s.system_prompt_patch;
    }
    if (s.name == "dynamic_workflow_harness") {
      found_dynamic_workflow_harness = true;
      dynamic_workflow_harness_prompt = s.system_prompt_patch;
    }
    EXPECT_FALSE(s.system_prompt_patch.empty());
  }
  EXPECT_EQ(skills->size(), 9);
  EXPECT_TRUE(found_planner);
  EXPECT_TRUE(found_code_reviewer);
  EXPECT_TRUE(found_patcher);
  EXPECT_TRUE(found_self_improvement_learner);
  EXPECT_TRUE(found_subagent_creator);
  EXPECT_TRUE(found_dynamic_workflow_harness);
  EXPECT_TRUE(absl::StrContains(patcher_prompt, "/review mail"));
  EXPECT_TRUE(absl::StrContains(patcher_prompt, "Do NOT declare completion"));
  EXPECT_TRUE(absl::StrContains(self_improvement_learner_prompt, "tools.persist_function(args)"));
  EXPECT_TRUE(absl::StrContains(self_improvement_learner_prompt, "successful `run_js` calls"));
  EXPECT_TRUE(absl::StrContains(subagent_creator_prompt, "[llm_tool_<suffix>]"));
  EXPECT_TRUE(absl::StrContains(subagent_creator_prompt, "ask_user"));
  EXPECT_TRUE(absl::StrContains(subagent_creator_prompt, "restart"));
  EXPECT_TRUE(absl::StrContains(dynamic_workflow_harness_prompt, "run_js"));
  EXPECT_TRUE(absl::StrContains(dynamic_workflow_harness_prompt, "tools.llm_query"));
  EXPECT_TRUE(absl::StrContains(dynamic_workflow_harness_prompt, "bounded repository survey"));
  EXPECT_TRUE(absl::StrContains(dynamic_workflow_harness_prompt, "Never call `run_js` recursively"));
}
TEST(DatabaseTest, MessagePersistence) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Hi there!", "call_1").ok());
  ASSERT_TRUE(db.AppendMessage("s2", "user", "Different session").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), 2);
  EXPECT_EQ((*history)[0].role, "user");
  EXPECT_EQ((*history)[0].content, "Hello");
  EXPECT_EQ((*history)[1].role, "assistant");
  EXPECT_EQ((*history)[1].tool_call_id, "call_1");
  auto history2 = db.GetConversationHistory("s2");
  ASSERT_TRUE(history2.ok());
  EXPECT_EQ(history2->size(), 1);
}
TEST(DatabaseTest, CloneSession) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Set up source session
  ASSERT_TRUE(db.AppendMessage("source", "user", "Hello").ok());
  ASSERT_TRUE(db.AppendMessage("source", "assistant", "Hi").ok());
  ASSERT_TRUE(db.RecordUsage("source", "gpt-4", 10, 20).ok());
  // Clone it
  auto status = db.CloneSession("source", "target");
  EXPECT_TRUE(status.ok()) << status.message();
  // Verify target session metadata
  auto target_history = db.GetConversationHistory("target");
  ASSERT_TRUE(target_history.ok());
  EXPECT_EQ(target_history->size(), 2);
  EXPECT_EQ((*target_history)[0].content, "Hello");
  EXPECT_EQ((*target_history)[1].content, "Hi");

  auto usage = db.GetTotalUsage("target");
  ASSERT_TRUE(usage.ok());
  EXPECT_EQ(usage->total_tokens, 30);
  // Verify uniqueness check
  status = db.CloneSession("source", "target");
  EXPECT_EQ(status.code(), absl::StatusCode::kAlreadyExists);
  // Verify source existence check
  status = db.CloneSession("non_existent", "new_target");
  EXPECT_EQ(status.code(), absl::StatusCode::kNotFound);
}
TEST(DatabaseTest, CloneEmptySession) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Create a session but don't add anything
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('empty');").ok());
  ASSERT_TRUE(db.CloneSession("empty", "empty_clone").ok());
  auto history = db.GetConversationHistory("empty_clone");
  ASSERT_TRUE(history.ok());
  EXPECT_TRUE(history->empty());
}
TEST(DatabaseTest, CloneLargeSession) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Ensure 'large' exists in sessions table
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('large');").ok());
  const int kNumMessages = 100;
  for (int i = 0; i < kNumMessages; ++i) {
    ASSERT_TRUE(db.AppendMessage("large", "user", absl::StrCat("Message ", i)).ok());
  }
  ASSERT_TRUE(db.CloneSession("large", "large_clone").ok());
  auto history = db.GetConversationHistory("large_clone");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), kNumMessages);
  EXPECT_EQ((*history)[99].content, "Message 99");
}
TEST(DatabaseTest, CloneStressTest) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Ensure 'root' exists in sessions table
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('root');").ok());
  ASSERT_TRUE(db.AppendMessage("root", "user", "root message").ok());
  // Chain clones: root -> c1 -> c2 -> ... -> c10
  std::string last = "root";
  for (int i = 1; i <= 10; ++i) {
    std::string current = absl::StrCat("c", i);
    ASSERT_TRUE(db.CloneSession(last, current).ok());
    last = current;
  }
  auto history = db.GetConversationHistory("c10");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), 1);
  EXPECT_EQ((*history)[0].content, "root message");
  // Fan-out clones: root -> f1, root -> f2, ...
  for (int i = 1; i <= 10; ++i) {
    std::string current = absl::StrCat("f", i);
    ASSERT_TRUE(db.CloneSession("root", current).ok());
  }
  for (int i = 1; i <= 10; ++i) {
    auto history_fan = db.GetConversationHistory(absl::StrCat("f", i));
    ASSERT_TRUE(history_fan.ok());
    EXPECT_EQ(history_fan->size(), 1);
  }
}
TEST(DatabaseTest, CloneFullSession) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  std::string sid = "full_source";
  ASSERT_TRUE(db.AppendMessage(sid, "user", "msg").ok());
  ASSERT_TRUE(db.RecordUsage(sid, "model", 1, 1).ok());
  ASSERT_TRUE(db.SetActiveSkills(sid, {"skill1", "skill2"}).ok());
  ASSERT_TRUE(db.CloneSession(sid, "full_target").ok());
  // Verify all
  auto hist = db.GetConversationHistory("full_target");
  EXPECT_EQ(hist->size(), 1);
  EXPECT_EQ(hist->at(0).content, "msg");
  auto usage = db.GetTotalUsage("full_target");
  EXPECT_EQ(usage->total_tokens, 2);
  auto skills = db.GetActiveSkills("full_target");
  EXPECT_EQ(skills->size(), 2);
  EXPECT_EQ(skills->at(0), "skill1");
  EXPECT_EQ(skills->at(1), "skill2");
}
TEST(DatabaseTest, AccordionContextSettingsAndLatestPromptTokens) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto defaults_or = db.GetAccordionContextSettings("s1");
  ASSERT_TRUE(defaults_or.ok());
  EXPECT_EQ(defaults_or->retain_groups, 2);
  EXPECT_EQ(defaults_or->watermark_tokens, 350000);
  EXPECT_TRUE(defaults_or->epoch_start_group_id.empty());
  ASSERT_TRUE(db.SetAccordionContextSettings("s1", 3, 400000).ok());
  ASSERT_TRUE(db.SetAccordionEpochStartGroup("s1", "g2").ok());
  auto settings_or = db.GetAccordionContextSettings("s1");
  ASSERT_TRUE(settings_or.ok());
  EXPECT_EQ(settings_or->retain_groups, 3);
  EXPECT_EQ(settings_or->watermark_tokens, 400000);
  EXPECT_EQ(settings_or->epoch_start_group_id, "g2");
  EXPECT_FALSE(db.SetAccordionContextSettings("s1", 0, 1).ok());
  EXPECT_FALSE(db.SetAccordionContextSettings("s1", 1, 0).ok());
  auto latest_or = db.GetLatestPromptTokens("s1");
  ASSERT_TRUE(latest_or.ok());
  EXPECT_FALSE(latest_or->has_value());
  ASSERT_TRUE(db.RecordUsage("s1", "model", 100, 10).ok());
  ASSERT_TRUE(db.RecordUsage("other", "model", 900, 10).ok());
  ASSERT_TRUE(db.RecordUsage("s1", "model", 350000, 10).ok());
  latest_or = db.GetLatestPromptTokens("s1");
  ASSERT_TRUE(latest_or.ok());
  ASSERT_TRUE(latest_or->has_value());
  EXPECT_EQ(**latest_or, 350000);
}
TEST(DatabaseTest, TokenPersistence) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello", "", "completed", "g1", "", 10).ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Hi", "", "completed", "g1", "", 25).ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 2);
  EXPECT_EQ((*history)[0].tokens, 10);
  EXPECT_EQ((*history)[1].tokens, 25);
}

TEST(DatabaseTest, CloneSessionThroughGroupCopiesPrefixOnly) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("other", "user", "interleaved", "", "completed", "other_g").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u3", "", "completed", "g3").ok());
  ASSERT_TRUE(db.UpdateMessageStatus(3, "dropped").ok());
  ASSERT_TRUE(db.SetScratchpad("s1", "scratch").ok());

  ASSERT_TRUE(db.CloneSessionThroughGroup("s1", "s2", "g2").ok());

  auto history = db.GetConversationHistory("s2");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 3);
  EXPECT_EQ((*history)[0].content, "u1");
  EXPECT_EQ((*history)[1].content, "a1");
  EXPECT_EQ((*history)[2].content, "a2");
  auto scratchpad = db.GetScratchpad("s2");
  ASSERT_TRUE(scratchpad.ok());
  EXPECT_EQ(*scratchpad, "scratch");
}

TEST(DatabaseTest, CloneSessionThroughGroupRejectsMissingGroupAndExistingTarget) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("target", "user", "existing").ok());

  EXPECT_FALSE(db.CloneSessionThroughGroup("s1", "new_target", "missing").ok());
  EXPECT_FALSE(db.CloneSessionThroughGroup("s1", "target", "g1").ok());
}

TEST(DatabaseTest, RollbackSessionToGroupDropsLaterMessages) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("other", "user", "interleaved", "", "completed", "other_g").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u3", "", "completed", "g3").ok());

  ASSERT_TRUE(db.RollbackSessionToGroup("s1", "g2").ok());

  auto visible = db.GetConversationHistory("s1");
  ASSERT_TRUE(visible.ok());
  ASSERT_EQ(visible->size(), 4);
  EXPECT_EQ((*visible)[3].content, "a2");
  auto all = db.GetConversationHistory("s1", true);
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->size(), 5);
  EXPECT_EQ((*all)[4].status, "dropped");
}

TEST(DatabaseTest, GetConversationHistoryWindowed) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // Create 3 groups of messages
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Resp 1", "", "completed", "g1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Resp 2", "", "completed", "g2").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 3", "", "completed", "g3").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "Resp 3", "", "completed", "g3").ok());
  // Add a message with NO group_id (should ALWAYS be included)
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Global Msg").ok());
  // Window size 2 should return Msg 2, Resp 2, Msg 3, Resp 3 (latest 2 groups) + Global Msg
  auto history = db.GetConversationHistory("s1", false, 2);
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 5);
  EXPECT_EQ((*history)[0].content, "Msg 2");
  EXPECT_EQ((*history)[1].content, "Resp 2");
  EXPECT_EQ((*history)[2].content, "Msg 3");
  EXPECT_EQ((*history)[3].content, "Resp 3");
  EXPECT_EQ((*history)[4].content, "Global Msg");
  // Window size 1 should return Msg 3, Resp 3 + Global Msg
  auto history1 = db.GetConversationHistory("s1", false, 1);
  ASSERT_TRUE(history1.ok());
  ASSERT_EQ(history1->size(), 3);
  EXPECT_EQ((*history1)[0].content, "Msg 3");
  EXPECT_EQ((*history1)[1].content, "Resp 3");
  EXPECT_EQ((*history1)[2].content, "Global Msg");
  // Window size 0 or large should return all
  auto historyall = db.GetConversationHistory("s1", false, 0);
  ASSERT_TRUE(historyall.ok());
  EXPECT_EQ(historyall->size(), 7);
}
TEST(DatabaseTest, GetConversationHistoryWindowedWithDropped) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  // g1: kept
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 1", "", "completed", "g1").ok());
  // g2: dropped
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 2", "", "dropped", "g2").ok());
  // g3: kept
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Msg 3", "", "completed", "g3").ok());
  // Window size 2, include_dropped=false
  // Should skip g2, and take latest 2 kept groups (g1, g3)
  auto history = db.GetConversationHistory("s1", false, 2);
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 2);
  EXPECT_EQ((*history)[0].content, "Msg 1");
  EXPECT_EQ((*history)[1].content, "Msg 3");
  // Window size 2, include_dropped=true
  // Should include g2, and take latest 2 groups (g2, g3)
  auto history_inc = db.GetConversationHistory("s1", true, 2);
  ASSERT_TRUE(history_inc.ok());
  ASSERT_EQ(history_inc->size(), 2);
  EXPECT_EQ((*history_inc)[0].content, "Msg 2");
  EXPECT_EQ((*history_inc)[1].content, "Msg 3");
}
TEST(DatabaseTest, UpdateMessageStatusWorks) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "Hello").ok());
  auto history = db.GetConversationHistory("s1");
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history->size(), 1);
  int msg_id = (*history)[0].id;
  EXPECT_EQ((*history)[0].status, "completed");
  ASSERT_TRUE(db.UpdateMessageStatus(msg_id, "dropped").ok());
  auto history2 = db.GetConversationHistory("s1", true);
  ASSERT_TRUE(history2.ok());
  ASSERT_EQ(history2->size(), 1);
  EXPECT_EQ((*history2)[0].status, "dropped");
  auto history3 = db.GetConversationHistory("s1", false);
  ASSERT_TRUE(history3.ok());
  EXPECT_EQ(history3->size(), 0);
}
TEST(DatabaseTest, GenericQuery) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto res = db.Query("SELECT 42 as answer, 'slop' as name");
  ASSERT_TRUE(res.ok());
  nlohmann::json j = slop::json_parse(*res).value_or(nlohmann::json::object());
  ASSERT_FALSE(j.is_discarded());
  ASSERT_EQ(j.size(), 1);
  EXPECT_EQ(j[0]["answer"], 42);
  EXPECT_EQ(j[0]["name"], "slop");
}
TEST(DatabaseTest, UsageTracking) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RecordUsage("s1", "model-a", 10, 20).ok());
  ASSERT_TRUE(db.RecordUsage("s1", "model-a", 5, 5).ok());
  ASSERT_TRUE(db.RecordUsage("s2", "model-b", 100, 200).ok());
  auto s1_usage = db.GetTotalUsage("s1");
  ASSERT_TRUE(s1_usage.ok());
  EXPECT_EQ(s1_usage->prompt_tokens, 15);
  EXPECT_EQ(s1_usage->completion_tokens, 25);
  EXPECT_EQ(s1_usage->total_tokens, 40);
  auto global_usage = db.GetTotalUsage();
  ASSERT_TRUE(global_usage.ok());
  EXPECT_EQ(global_usage->prompt_tokens, 115);
  EXPECT_EQ(global_usage->completion_tokens, 225);
  EXPECT_EQ(global_usage->total_tokens, 340);
}
TEST(DatabaseTest, SkillTracking) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  slop::Database::Skill skill;
  skill.name = "test_skill";
  skill.description = "desc";
  skill.system_prompt_patch = "patch";
  ASSERT_TRUE(db.RegisterSkill(skill).ok());
  // Test Activation Count
  ASSERT_TRUE(db.IncrementSkillActivationCount("test_skill").ok());
  ASSERT_TRUE(db.IncrementSkillActivationCount("test_skill").ok());
  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  bool found = false;
  for (const auto& s : *skills) {
    if (s.name == "test_skill") {
      EXPECT_EQ(s.activation_count, 2);
      found = true;
    }
  }
  EXPECT_TRUE(found);
  // Test Session Skill Persistence
  std::vector<std::string> active = {"skill1", "skill2"};
  // Ensure session exists
  ASSERT_TRUE(db.SetAccordionContextSettings("s1", 2, 350000).ok());
  ASSERT_TRUE(db.SetActiveSkills("s1", active).ok());
  auto restored = db.GetActiveSkills("s1");
  ASSERT_TRUE(restored.ok());
  ASSERT_EQ(restored->size(), 2);
  EXPECT_EQ((*restored)[0], "skill1");
  EXPECT_EQ((*restored)[1], "skill2");
}
TEST(DatabaseTest, JsFunctionsTableExistsForPersistedRunJsHelpers) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto funcs_or = db.Query("SELECT name, json_schema FROM js_functions ORDER BY name");
  ASSERT_TRUE(funcs_or.ok()) << funcs_or.status();
  auto funcs = slop::json_parse(*funcs_or);
  ASSERT_TRUE(funcs.has_value());
  ASSERT_TRUE(funcs->is_array());
  EXPECT_TRUE(funcs->empty());
}

TEST(DatabaseTest, DefaultCppToolSchemasMatchCurrentContracts) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto tools_or = db.Query(
      "SELECT name, json_schema FROM tools WHERE name IN ('query_db','run_js','git_commit_patch') ORDER BY "
      "name");
  ASSERT_TRUE(tools_or.ok());
  auto rows = slop::json_parse(*tools_or).value_or(nlohmann::json::array());
  ASSERT_TRUE(rows.is_array());
  ASSERT_EQ(rows.size(), 3);

  std::map<std::string, nlohmann::json> by_name;
  for (const auto& row : rows) {
    ASSERT_TRUE(row.is_object());
    const std::string name = row.value("name", "");
    auto schema = slop::json_parse(row.value("json_schema", std::string{})).value_or(nlohmann::json::object());
    ASSERT_TRUE(schema.is_object());
    by_name[name] = schema;
  }

  ASSERT_TRUE(by_name.find("query_db") != by_name.end());
  ASSERT_TRUE(by_name["query_db"]["properties"].contains("params"));
  EXPECT_TRUE(by_name["query_db"]["properties"]["params"].contains("items"));

  ASSERT_TRUE(by_name.find("git_commit_patch") != by_name.end());
  EXPECT_TRUE(by_name["git_commit_patch"]["properties"].contains("summary"));
  EXPECT_FALSE(by_name["git_commit_patch"]["properties"].contains("message"));

  ASSERT_TRUE(by_name.find("run_js") != by_name.end());
  EXPECT_TRUE(by_name["run_js"]["properties"].contains("code"));
  EXPECT_TRUE(by_name["run_js"]["properties"].contains("input"));
  EXPECT_TRUE(by_name["run_js"]["required"].is_array());
}

TEST(DatabaseTest, ToolUsageCounters) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  slop::Database::Tool tool = {"test_tool", "desc", "{}", true};
  ASSERT_TRUE(db.RegisterTool(tool).ok());
  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  auto it = std::find_if(tools->begin(), tools->end(), [](const auto& t) { return t.name == "test_tool"; });
  ASSERT_NE(it, tools->end());
  EXPECT_EQ(it->call_count, 0);
  ASSERT_TRUE(db.IncrementToolCallCount("test_tool").ok());
  ASSERT_TRUE(db.IncrementToolCallCount("test_tool").ok());
  tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  it = std::find_if(tools->begin(), tools->end(), [](const auto& t) { return t.name == "test_tool"; });
  ASSERT_NE(it, tools->end());
  EXPECT_EQ(it->call_count, 2);
  // Registering again should preserve count
  tool.description = "updated desc";
  ASSERT_TRUE(db.RegisterTool(tool).ok());
  tools = db.GetEnabledTools();
  it = std::find_if(tools->begin(), tools->end(), [](const auto& t) { return t.name == "test_tool"; });
  EXPECT_EQ(it->call_count, 2);
  EXPECT_EQ(it->description, "updated desc");
}

TEST(DatabaseTest, PromptMaterialIsOrderedByName) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.RegisterTool({"zulu", "desc", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterTool({"alpha", "desc", "{}", true}).ok());
  ASSERT_TRUE(db.RegisterSkill({0, "zulu", "desc", "patch"}).ok());
  ASSERT_TRUE(db.RegisterSkill({0, "alpha", "desc", "patch"}).ok());

  auto tools = db.GetTopLevelTools();
  ASSERT_TRUE(tools.ok());
  auto alpha_tool = std::find_if(tools->begin(), tools->end(), [](const auto& tool) { return tool.name == "alpha"; });
  auto zulu_tool = std::find_if(tools->begin(), tools->end(), [](const auto& tool) { return tool.name == "zulu"; });
  ASSERT_NE(alpha_tool, tools->end());
  ASSERT_NE(zulu_tool, tools->end());
  EXPECT_LT(alpha_tool, zulu_tool);

  auto skills = db.GetSkills();
  ASSERT_TRUE(skills.ok());
  auto alpha_skill = std::find_if(skills->begin(), skills->end(), [](const auto& skill) { return skill.name == "alpha"; });
  auto zulu_skill = std::find_if(skills->begin(), skills->end(), [](const auto& skill) { return skill.name == "zulu"; });
  ASSERT_NE(alpha_skill, skills->end());
  ASSERT_NE(zulu_skill, skills->end());
  EXPECT_LT(alpha_skill, zulu_skill);
}

TEST(DatabaseTest, ScratchpadReadWrite) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  ASSERT_TRUE(db.SetScratchpad("s1", "plan v1").ok());
  auto content_or = db.GetScratchpad("s1");
  ASSERT_TRUE(content_or.ok());
  EXPECT_EQ(*content_or, "plan v1");

  ASSERT_TRUE(db.SetScratchpad("s1", "plan v2").ok());
  content_or = db.GetScratchpad("s1");
  ASSERT_TRUE(content_or.ok());
  EXPECT_EQ(*content_or, "plan v2");
}

TEST(DatabaseTest, GetLastAssistantMessage) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "u1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a1").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "assistant", "a2").ok());

  auto last_or = db.GetLastAssistantMessage("s1");
  ASSERT_TRUE(last_or.ok());
  EXPECT_EQ(*last_or, "a2");
}

TEST(DatabaseTest, CloneSessionCopiesScratchpad) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.AppendMessage("s1", "user", "hello").ok());
  ASSERT_TRUE(db.SetScratchpad("s1", "session plan").ok());

  ASSERT_TRUE(db.CloneSession("s1", "s2").ok());
  auto content_or = db.GetScratchpad("s2");
  ASSERT_TRUE(content_or.ok());
  EXPECT_EQ(*content_or, "session plan");
}

TEST(DatabaseTest, ConcurrentAccess) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  const int num_threads = 10;
  const int iterations = 100;
  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&db, &success_count, i]() {
      for (int j = 0; j < iterations; ++j) {
        std::string session = absl::StrCat("session_", i);
        std::string content = absl::StrCat("content_", j);
        auto status = db.AppendMessage(session, "user", content);
        if (status.ok()) {
          success_count++;
        }
        // Use an existing method that is thread-safe (locks mu_)
        auto history = db.GetSkills();
        if (history.ok()) {
          success_count++;
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(success_count, num_threads * iterations * 2);
}
