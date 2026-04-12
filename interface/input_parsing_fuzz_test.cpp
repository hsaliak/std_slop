
#include "interface/input_parsing.h"

#include <string>

#include <array>

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

void InputWithoutSlashAfterLeadingWhitespaceIsNotCommand(const std::string& leading,
                                                         const std::string& rest) {
  std::string input = leading + rest;
  absl::string_view trimmed = absl::StripLeadingAsciiWhitespace(input);
  if (!trimmed.empty() && trimmed[0] == '/') {
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

bool IsLikelyHardBlockedCommandPrefix(absl::string_view command) {
  constexpr std::array<absl::string_view, 10> kBlocked = {
      "/edit", "/exec", "/feedback", "/undo", "/exit", "/review", "/scratchpad", "/session", "/message", "/quit",
  };
  for (absl::string_view prefix : kBlocked) {
    if (command == prefix || absl::StartsWith(command, prefix)) {
      return true;
    }
  }
  return false;
}

void HardBlockedCommandPrefixParsingRemainsStable(const std::string& command_part, const std::string& suffix) {
  const std::string raw = " /" + command_part + suffix;
  const ParsedCommand parsed = ParseCommandInput(raw);
  if (!parsed.is_command) {
    return;
  }
  if (!IsLikelyHardBlockedCommandPrefix(parsed.command)) {
    return;
  }
  EXPECT_TRUE(absl::StartsWith(parsed.command, "/"));
  EXPECT_FALSE(parsed.command.empty());
}

FUZZ_TEST(InputParsingFuzzTest, ParseCommandNeverCrashes);

FUZZ_TEST(InputParsingFuzzTest, InputWithoutSlashAfterLeadingWhitespaceIsNotCommand)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

FUZZ_TEST(InputParsingFuzzTest, CommandRoundTripCanonical)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_\\-]{0,16}"), fuzztest::InRegexp("[\\x20-\\x7E]{0,128}"));

FUZZ_TEST(InputParsingFuzzTest, HardBlockedCommandPrefixParsingRemainsStable)
    .WithDomains(fuzztest::InRegexp("(edit|exec|feedback|undo|exit|review|scratchpad|session|message|quit)[A-Za-z0-9_\\-]{0,12}"),
                 fuzztest::InRegexp("[\\x20-\\x7E]{0,64}"));

}  // namespace
}  // namespace slop