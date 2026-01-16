#include <gtest/gtest.h>
#include "command_handler.h"
#include "database.h"
#include <vector>
#include <string>

namespace slop {

class InputParsingTest : public ::testing::Test {
protected:
    Database db;
    std::string session_id = "test_session";
    std::vector<std::string> active_skills;

    void SetUp() override {
        db.Init(":memory:");
    }
};

TEST_F(InputParsingTest, SquareBracesInNormalInput) {
    CommandHandler handler(&db, nullptr, nullptr, "", "");
    std::string input = "This is a test [with square braces]";
    auto result = handler.Handle(input, session_id, active_skills, [](){}, {});
    
    // Should NOT be handled as a command
    EXPECT_EQ(result, CommandHandler::Result::NOT_A_COMMAND);
}

TEST_F(InputParsingTest, SquareBracesInCommandArgs) {
    CommandHandler handler(&db, nullptr, nullptr, "", "");
    // Many commands just take the rest of the line as args
    std::string input = "/session session[1]";
    auto result = handler.Handle(input, session_id, active_skills, [](){}, {});
    
    EXPECT_EQ(result, CommandHandler::Result::HANDLED);
    EXPECT_EQ(session_id, "session[1]");
}

TEST_F(InputParsingTest, SingleQuotesInCommandArgs) {
    CommandHandler handler(&db, nullptr, nullptr, "", "");
    // This tests if the manual SQL construction fails or is vulnerable
    std::string input = "/session session' OR '1'='1";
    auto result = handler.Handle(input, session_id, active_skills, [](){}, {});
    
    EXPECT_EQ(result, CommandHandler::Result::HANDLED);
    EXPECT_EQ(session_id, "session' OR '1'='1");
}

TEST_F(InputParsingTest, MalformedCommand) {
    CommandHandler handler(&db, nullptr, nullptr, "", "");
    std::string input = "/nonexistent_command [arg]";
    auto result = handler.Handle(input, session_id, active_skills, [](){}, {});
    
    EXPECT_EQ(result, CommandHandler::Result::UNKNOWN);
}

} // namespace slop
