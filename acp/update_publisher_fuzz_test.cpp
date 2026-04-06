
#include "acp/update_publisher.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop::acp {
namespace {

void SessionUpdateNotificationShapeStable(const std::string& session_id, int state_raw) {
  const SessionUpdateState state = static_cast<SessionUpdateState>(state_raw % 5);
  const nlohmann::json notification = MakeSessionUpdateNotification(session_id, state);

  EXPECT_EQ(notification.at("jsonrpc").get<std::string>(), "2.0");
  EXPECT_EQ(notification.at("method").get<std::string>(), "session/update");
  ASSERT_TRUE(notification.at("params").is_object());
  EXPECT_EQ(notification.at("params").at("sessionId").get<std::string>(), session_id);
  EXPECT_FALSE(notification.at("params").at("state").get<std::string>().empty());
}

FUZZ_TEST(UpdatePublisherFuzzTest, SessionUpdateNotificationShapeStable);

}  // namespace
}  // namespace slop::acp
