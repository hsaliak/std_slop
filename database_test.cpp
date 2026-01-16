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

TEST(DatabaseTest, SkillsPersistence) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    slop::Database::Skill skill = {1, "expert", "Expert skill", "PATCH"};
    ASSERT_TRUE(db.RegisterSkill(skill).ok());
    
    auto skills = db.GetSkills();
    ASSERT_TRUE(skills.ok());
    ASSERT_EQ(skills->size(), 1); 
    
    EXPECT_EQ((*skills)[0].name, "expert");
}

TEST(DatabaseTest, ContextSettingsPersistence) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    ASSERT_TRUE(db.SetContextWindow("s1", 15).ok());
    
    auto settings = db.GetContextSettings("s1");
    ASSERT_TRUE(settings.ok());
    EXPECT_EQ(settings->size, 15);
}

TEST(DatabaseTest, GenericQuery) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    auto res = db.Query("SELECT 42 as answer, 'slop' as name");
    ASSERT_TRUE(res.ok());
    
    nlohmann::json j = nlohmann::json::parse(*res);
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
