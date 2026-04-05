
#include "acp/session_store.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(SessionStoreTest, ParseSessionNewParamsAcceptsEmptyObject) {
  auto parsed_or = ParseSessionNewParams(nlohmann::json::object());
  ASSERT_TRUE(parsed_or.ok());
  EXPECT_FALSE(parsed_or->session_id.has_value());
}

TEST(SessionStoreTest, ParseSessionNewParamsRejectsNonObject) {
  auto parsed_or = ParseSessionNewParams(nlohmann::json::array());
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_new_params_must_be_object");
}

TEST(SessionStoreTest, ParseSessionNewParamsRejectsMalformedId) {
  nlohmann::json params = {
      {"sessionId", "bad id"},
  };
  auto parsed_or = ParseSessionNewParams(params);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().message(), "session_new_session_id_invalid");
}

TEST(SessionStoreTest, CreateSessionUsesProvidedId) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  SessionNewRequest req;
  req.session_id = "acp_session_1";
  auto session_id_or = CreateSession(&db, req);
  ASSERT_TRUE(session_id_or.ok());
  EXPECT_EQ(*session_id_or, "acp_session_1");

  auto count_or = db.GetContextSettings("acp_session_1");
  EXPECT_TRUE(count_or.ok());
}

TEST(SessionStoreTest, CreateSessionGeneratesDefaultId) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  SessionNewRequest req;
  auto session_id_or = CreateSession(&db, req);
  ASSERT_TRUE(session_id_or.ok());
  EXPECT_TRUE(IsValidSessionId(*session_id_or));

  auto count_or = db.GetContextSettings(*session_id_or);
  EXPECT_TRUE(count_or.ok());
}

TEST(SessionStoreTest, CreateSessionRejectsProvidedDuplicateId) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());

  SessionNewRequest req;
  req.session_id = "acp_dup_1";
  auto first_or = CreateSession(&db, req);
  ASSERT_TRUE(first_or.ok());

  auto second_or = CreateSession(&db, req);
  ASSERT_FALSE(second_or.ok());
  EXPECT_EQ(second_or.status().message(), "session_new_session_id_exists");
}

TEST(SessionStoreTest, CreateSessionRetriesOnGeneratedIdCollision) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp-40')").ok());
  ASSERT_TRUE(db.Execute("INSERT INTO sessions (id) VALUES ('acp-41')").ok());

  SessionNewRequest req;
  auto next_or = CreateSession(&db, req);
  ASSERT_TRUE(next_or.ok());
  EXPECT_EQ(*next_or, "acp-42");

  auto next_exists_or = db.GetContextSettings(*next_or);
  EXPECT_TRUE(next_exists_or.ok());
}


TEST(SessionStoreTest, SessionIdValidationRules) {
  EXPECT_TRUE(IsValidSessionId("abc"));
  EXPECT_TRUE(IsValidSessionId("a-b_c-123"));
  EXPECT_FALSE(IsValidSessionId(""));
  EXPECT_FALSE(IsValidSessionId("has space"));
  EXPECT_FALSE(IsValidSessionId("x/y"));
}

}  // namespace
}  // namespace slop::acp