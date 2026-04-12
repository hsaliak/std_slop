
#include "acp/update_publisher.h"

#include <gtest/gtest.h>

namespace slop::acp {
namespace {

TEST(UpdatePublisherTest, StateToStringCoversAllStates) {
  EXPECT_EQ(SessionUpdateStateToString(SessionUpdateState::kAccepted), "accepted");
  EXPECT_EQ(SessionUpdateStateToString(SessionUpdateState::kStarted), "started");
  EXPECT_EQ(SessionUpdateStateToString(SessionUpdateState::kExecutingTools), "executing_tools");
  EXPECT_EQ(SessionUpdateStateToString(SessionUpdateState::kCompleted), "completed");
  EXPECT_EQ(SessionUpdateStateToString(SessionUpdateState::kCancelled), "cancelled");
}

TEST(UpdatePublisherTest, StateToStringReturnsUnknownForOutOfRangeValue) {
  EXPECT_EQ(SessionUpdateStateToString(static_cast<SessionUpdateState>(999)), "unknown");
}

TEST(UpdatePublisherTest, NotificationShapeMatchesSessionUpdateMethod) {
  const nlohmann::json notification = MakeSessionUpdateNotification("acp_1", SessionUpdateState::kStarted);

  EXPECT_EQ(notification.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_EQ(notification.at("method").get<std::string>(), "session/update");
  ASSERT_TRUE(notification.at("params").is_object());
  EXPECT_EQ(notification.at("params").at("sessionId").get<std::string>(), "acp_1");
  ASSERT_TRUE(notification.at("params").at("update").is_object());
  EXPECT_EQ(notification.at("params").at("update").at("sessionUpdate").get<std::string>(), "agent_thought_chunk");
  EXPECT_EQ(notification.at("params").at("update").at("content").at("type").get<std::string>(), "text");
  EXPECT_EQ(notification.at("params").at("update").at("content").at("text").get<std::string>(), "started");
  EXPECT_FALSE(notification.contains("id"));
}

TEST(UpdatePublisherTest, NotificationMapsCompletedToAgentMessageChunk) {
  const nlohmann::json notification = MakeSessionUpdateNotification("acp_1", SessionUpdateState::kCompleted);
  EXPECT_EQ(notification.at("params").at("update").at("sessionUpdate").get<std::string>(), "agent_message_chunk");
  EXPECT_EQ(notification.at("params").at("update").at("content").at("text").get<std::string>(), "completed");
}

}  // namespace
}  // namespace slop::acp
