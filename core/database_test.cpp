#include "core/database.h"
#include "absl/strings/str_cat.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "json_utils.h"
#include <atomic>
#include <thread>
#include <vector>

TEST(DatabaseTest, InitWorks) {
  slop::Database db;
  auto status = db.Init(":memory:");
  EXPECT_TRUE(status.ok()) << status.message();
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
  bool found_planner = false;
  bool found_code_reviewer = false;
  for (const auto& s : *skills) {
    if (s.name == "planner") found_planner = true;
    if (s.name == "code_reviewer") found_code_reviewer = true;
  }
  EXPECT_TRUE(found_planner);
  EXPECT_TRUE(found_code_reviewer);
  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  // Only query_db and run_lua should be registered and enabled
  EXPECT_EQ(tools->size(), 2);
  bool found_run_lua = false;
  bool found_query_db = false;
  bool found_read_file = false;
  for (const auto& t : *tools) {
    if (t.name == "run_lua") found_run_lua = true;
    if (t.name == "query_db") found_query_db = true;
    if (t.name == "read_file") found_read_file = true;
  }
  EXPECT_TRUE(found_run_lua);
  EXPECT_TRUE(found_query_db);
  EXPECT_FALSE(found_read_file);
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
  ASSERT_TRUE(db.UpdateScratchpad("source", "original scratchpad").ok());
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
  auto scratchpad = db.GetScratchpad("target");
  ASSERT_TRUE(scratchpad.ok());
  EXPECT_EQ(*scratchpad, "original scratchpad");
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
  auto scratch = db.GetScratchpad("empty_clone");
  ASSERT_TRUE(scratch.ok());
  EXPECT_TRUE(scratch->empty());
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
  ASSERT_TRUE(db.UpdateScratchpad("large", std::string(1000, 'x')).ok());
  ASSERT_TRUE(db.CloneSession("large", "large_clone").ok());
  auto history = db.GetConversationHistory("large_clone");
  ASSERT_TRUE(history.ok());
  EXPECT_EQ(history->size(), kNumMessages);
  EXPECT_EQ((*history)[99].content, "Message 99");
  auto scratch = db.GetScratchpad("large_clone");
  ASSERT_TRUE(scratch.ok());
  EXPECT_EQ(scratch->size(), 1000);
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
  ASSERT_TRUE(db.UpdateScratchpad(sid, "scratch").ok());
  ASSERT_TRUE(db.AppendMessage(sid, "user", "msg").ok());
  ASSERT_TRUE(db.RecordUsage(sid, "model", 1, 1).ok());
  ASSERT_TRUE(db.SetSessionState(sid, "state blob").ok());
  ASSERT_TRUE(db.SetActiveSkills(sid, {"skill1", "skill2"}).ok());
  ASSERT_TRUE(db.CloneSession(sid, "full_target").ok());
  // Verify all
  EXPECT_EQ(*db.GetScratchpad("full_target"), "scratch");
  auto hist = db.GetConversationHistory("full_target");
  EXPECT_EQ(hist->size(), 1);
  EXPECT_EQ(hist->at(0).content, "msg");
  auto usage = db.GetTotalUsage("full_target");
  EXPECT_EQ(usage->total_tokens, 2);
  EXPECT_EQ(*db.GetSessionState("full_target"), "state blob");
  auto skills = db.GetActiveSkills("full_target");
  EXPECT_EQ(skills->size(), 2);
  EXPECT_EQ(skills->at(0), "skill1");
  EXPECT_EQ(skills->at(1), "skill2");
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
  ASSERT_TRUE(db.SetContextWindow("s1", 10).ok());
  ASSERT_TRUE(db.SetActiveSkills("s1", active).ok());
  auto restored = db.GetActiveSkills("s1");
  ASSERT_TRUE(restored.ok());
  ASSERT_EQ(restored->size(), 2);
  EXPECT_EQ((*restored)[0], "skill1");
  EXPECT_EQ((*restored)[1], "skill2");
}
TEST(DatabaseTest, ApplyPatchToolSchema) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  bool found = false;
  for (const auto& t : *tools) {
    if (t.name == "apply_patch") {
      found = true;
      nlohmann::json schema = slop::json_parse(t.json_schema).value_or(nlohmann::json::object());
      ASSERT_FALSE(schema.is_discarded());
      EXPECT_EQ(schema["type"], "object");
      EXPECT_TRUE(schema["properties"].contains("path"));
      EXPECT_TRUE(schema["properties"].contains("patches"));
      EXPECT_EQ(schema["properties"]["patches"]["type"], "array");
      auto item_props = schema["properties"]["patches"]["items"]["properties"];
      EXPECT_TRUE(item_props.contains("find"));
      EXPECT_TRUE(item_props.contains("replace"));
    }
  }
  EXPECT_FALSE(found) << "apply_patch found in enabled tools";
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
