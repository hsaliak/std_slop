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

TEST(DatabaseTest, FTS5Works) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    EXPECT_TRUE(db.Execute("INSERT INTO code_search (path, content) VALUES ('main.cpp', 'int main() {}')").ok());
    // FTS5 specific query
    EXPECT_TRUE(db.Execute("SELECT * FROM code_search WHERE code_search MATCH 'main'").ok());
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
    
    slop::Database::Skill skill = {1, "expert", "Expert skill", "PATCH", "[]"};
    ASSERT_TRUE(db.RegisterSkill(skill).ok());
    
    auto skills = db.GetSkills();
    ASSERT_TRUE(skills.ok());
    ASSERT_EQ(skills->size(), 1);
    EXPECT_EQ((*skills)[0].name, "expert");
    EXPECT_EQ((*skills)[0].system_prompt_patch, "PATCH");
}

TEST(DatabaseTest, GroupSearchWorks) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    ASSERT_TRUE(db.IndexGroup("g1", "the quick brown fox").ok());
    ASSERT_TRUE(db.IndexGroup("g2", "jumps over the lazy dog").ok());
    
    auto results = db.SearchGroups("fox", 10);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 1);
    EXPECT_EQ((*results)[0], "g1");
}

TEST(DatabaseTest, ContextSettingsPersistence) {
    slop::Database db;
    ASSERT_TRUE(db.Init(":memory:").ok());
    
    ASSERT_TRUE(db.SetContextMode("s1", slop::Database::ContextMode::FTS_RANKED, 15).ok());
    
    auto settings = db.GetContextSettings("s1");
    ASSERT_TRUE(settings.ok());
    EXPECT_EQ(settings->mode, slop::Database::ContextMode::FTS_RANKED);
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
