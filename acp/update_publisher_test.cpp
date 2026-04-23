
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

TEST(UpdatePublisherTest, ToolCallUpdateNotificationUsesStructuredShape) {
  const nlohmann::json notification =
      MakeToolCallUpdateNotification("acp_1", "call_1", "echo", "completed", "done");

  const auto& update = notification.at("params").at("update");
  EXPECT_EQ(update.at("sessionUpdate").get<std::string>(), "tool_call_update");
  EXPECT_EQ(update.at("toolCallId").get<std::string>(), "call_1");
  EXPECT_EQ(update.at("title").get<std::string>(), "echo");
  EXPECT_EQ(update.at("status").get<std::string>(), "completed");
  ASSERT_TRUE(update.contains("content"));
  ASSERT_TRUE(update.at("content").is_array());
  EXPECT_EQ(update.at("content").at(0).at("type").get<std::string>(), "text");
  EXPECT_EQ(update.at("content").at(0).at("text").get<std::string>(), "done");
}

}  // namespace
}  // namespace slop::acp
