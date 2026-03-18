
#include "interface/input_parsing.h"

#include <string>

#include "absl/strings/ascii.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

void ParseCommandNeverCrashes(const std::string& input) {
  const ParsedCommand parsed = ParseCommandInput(input);
  if (!parsed.is_command) {
    EXPECT_TRUE(parsed.command.empty());
  }
}

void NonSlashInputIsNotCommand(const std::string& leading, const std::string& rest) {
  std::string input = leading + rest;
  if (!input.empty() && input[0] == '/') {
    return;
  }
  const ParsedCommand parsed = ParseCommandInput(input);
  EXPECT_FALSE(parsed.is_command);
}

void CommandRoundTripCanonical(const std::string& command_suffix, const std::string& args) {
  const std::string command = std::string("/") + command_suffix;
  if (command == "/") {
    return;
  }
  const std::string rendered = RenderCommandInput(command, args);
  const ParsedCommand parsed = ParseCommandInput(rendered);

  EXPECT_TRUE(parsed.is_command);
  EXPECT_EQ(parsed.command, command);
  EXPECT_EQ(parsed.args, std::string(absl::StripAsciiWhitespace(args)));
}

FUZZ_TEST(InputParsingFuzzTest, ParseCommandNeverCrashes);

FUZZ_TEST(InputParsingFuzzTest, NonSlashInputIsNotCommand)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

FUZZ_TEST(InputParsingFuzzTest, CommandRoundTripCanonical)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_\\-]{0,16}"), fuzztest::InRegexp("[\\x20-\\x7E]{0,128}"));

}  // namespace
}  // namespace slop