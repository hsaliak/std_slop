#include "database.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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
  // We expect at least the 3 default skills we added
  EXPECT_GE(skills->size(), 3);

  bool found_planner = false;
  for (const auto& s : *skills) {
    if (s.name == "planner") found_planner = true;
  }
  EXPECT_TRUE(found_planner);

  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());
  // We expect at least the 7 default tools we added
  EXPECT_GE(tools->size(), 7);

  bool found_read_file = false;
  for (const auto& t : *tools) {
    if (t.name == "read_file") found_read_file = true;
  }
  EXPECT_TRUE(found_read_file);
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

  nlohmann::json j = nlohmann::json::parse(*res, nullptr, false);
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

TEST(DatabaseTest, ApplyPatchToolSchema) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  auto tools = db.GetEnabledTools();
  ASSERT_TRUE(tools.ok());

  bool found = false;
  for (const auto& t : *tools) {
    if (t.name == "apply_patch") {
      found = true;
      nlohmann::json schema = nlohmann::json::parse(t.json_schema);
      EXPECT_EQ(schema["type"], "object");
      EXPECT_TRUE(schema["properties"].contains("path"));
      EXPECT_TRUE(schema["properties"].contains("patches"));
      EXPECT_EQ(schema["properties"]["patches"]["type"], "array");
      auto item_props = schema["properties"]["patches"]["items"]["properties"];
      EXPECT_TRUE(item_props.contains("find"));
      EXPECT_TRUE(item_props.contains("replace"));
    }
  }
  EXPECT_TRUE(found) << "apply_patch tool not found in registered tools";
}

TEST(DatabaseTest, MemoStorageAndFiltering) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  // Test AddMemo
  EXPECT_TRUE(db.AddMemo("Memo 1", "[\"tag1\", \"tag2\"]").ok());
  EXPECT_TRUE(db.AddMemo("Memo 2", "[\"tag2\", \"tag3\"]").ok());
  EXPECT_TRUE(db.AddMemo("Memo 3", "[\"tag4\"]").ok());

  // Test GetAllMemos
  auto all_memos = db.GetAllMemos();
  ASSERT_TRUE(all_memos.ok());
  EXPECT_EQ(all_memos->size(), 3);

  // Test GetMemosByTags with single tag
  auto tag2_memos = db.GetMemosByTags({"tag2"});
  ASSERT_TRUE(tag2_memos.ok());
  EXPECT_EQ(tag2_memos->size(), 2);
  bool found1 = false, found2 = false;
  for (const auto& m : *tag2_memos) {
    if (m.content == "Memo 1") found1 = true;
    if (m.content == "Memo 2") found2 = true;
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);

  // Test GetMemosByTags with multiple tags
  auto multi_tag_memos = db.GetMemosByTags({"tag1", "tag4"});
  ASSERT_TRUE(multi_tag_memos.ok());
  EXPECT_EQ(multi_tag_memos->size(), 2);

  // Test GetMemosByTags with no matches
  auto no_match_memos = db.GetMemosByTags({"nonexistent"});
  ASSERT_TRUE(no_match_memos.ok());
  EXPECT_EQ(no_match_memos->size(), 0);
}

TEST(DatabaseTest, ExtractTags) {
  auto tags = slop::Database::ExtractTags("The quick brown fox jumps-over the lazy dog, arch-decision.");
  std::set<std::string> tag_set(tags.begin(), tags.end());

  // "The", "the" are stopwords
  // "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "arch", "decision" should be tags
  EXPECT_TRUE(tag_set.count("quick"));
  EXPECT_TRUE(tag_set.count("brown"));
  EXPECT_TRUE(tag_set.count("jumps"));
  EXPECT_TRUE(tag_set.count("over"));
  EXPECT_TRUE(tag_set.count("arch"));
  EXPECT_TRUE(tag_set.count("decision"));

  EXPECT_FALSE(tag_set.count("the"));
  EXPECT_FALSE(tag_set.count("dog"));  // "dog" is length 3, and word.length() > 3
}

TEST(DatabaseTest, MemoSearchCompoundTags) {
  slop::Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  nlohmann::json tags = {"arch-decision", "api-design"};
  ASSERT_TRUE(db.AddMemo("Complex architecture decisions", tags.dump()).ok());

  // Search by exact compound tag
  auto exact = db.GetMemosByTags({"arch-decision"});
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact->size(), 1);

  // Search by component
  auto partial = db.GetMemosByTags({"arch"});
  ASSERT_TRUE(partial.ok());
  EXPECT_EQ(partial->size(), 1);
}
