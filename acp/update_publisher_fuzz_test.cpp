
#include "acp/update_publisher.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void SessionUpdateNotificationShapeStable(const std::string& session_id, int state_raw) {
  const int normalized_state = ((state_raw % 5) + 5) % 5;
  const SessionUpdateState state = static_cast<SessionUpdateState>(normalized_state);
  const nlohmann::json notification = MakeSessionUpdateNotification(session_id, state);

  EXPECT_EQ(notification.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_EQ(notification.at("method").get<std::string>(), "session/update");
  ASSERT_TRUE(notification.at("params").is_object());
  EXPECT_EQ(notification.at("params").at("sessionId").get<std::string>(), session_id);
  ASSERT_TRUE(notification.at("params").at("update").is_object());
  EXPECT_FALSE(notification.at("params").at("update").at("sessionUpdate").get<std::string>().empty());
  ASSERT_TRUE(notification.at("params").at("update").at("content").is_object());
  EXPECT_EQ(notification.at("params").at("update").at("content").at("type").get<std::string>(), "text");
  EXPECT_FALSE(notification.at("params").at("update").at("content").at("text").get<std::string>().empty());
}

FUZZ_TEST(UpdatePublisherFuzzTest, SessionUpdateNotificationShapeStable);

}  // namespace
}  // namespace slop::acp
