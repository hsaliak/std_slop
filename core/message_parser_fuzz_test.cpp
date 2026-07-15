
#include "core/message_parser.h"

#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

Database::Message MakeMessage(const std::string& content, const std::string& status, const std::string& tool_call_id,
                              const std::string& parsing_strategy) {
  Database::Message msg;
  msg.id = 1;
  msg.session_id = "session";
  msg.role = "assistant";
  msg.content = content;
  msg.tool_call_id = tool_call_id;
  msg.status = status;
  msg.created_at = "";
  msg.group_id = "";
  msg.parsing_strategy = parsing_strategy;
  msg.tokens = 0;
  return msg;
}

void ExtractToolCallsIsTotal(const std::string& content, const std::string& status, const std::string& tool_call_id,
                             const std::string& parsing_strategy) {
  Database::Message msg = MakeMessage(content, status, tool_call_id, parsing_strategy);
  MessageContext ctx(msg);
  auto calls_or = MessageParser::ExtractToolCalls(ctx);

  if (!calls_or.ok()) {
    EXPECT_FALSE(calls_or.status().ok());
    return;
  }

  for (const ToolCall& call : *calls_or) {
    EXPECT_FALSE(call.name.empty());
    EXPECT_TRUE(call.args.is_null() || call.args.is_object() || call.args.is_array() || call.args.is_primitive());
  }
}

void NonToolCallAssistantTextPassesThrough(const std::string& content, const std::string& parsing_strategy) {
  Database::Message msg = MakeMessage(content, "assistant", "tool-1", parsing_strategy);
  MessageContext ctx(msg);
  EXPECT_EQ(MessageParser::ExtractAssistantText(ctx), content);
}

void ToolCallAssistantTextMatchesJsonContent(const std::string& text, const std::string& parsing_strategy,
                                             const std::string& tool_call_id) {
  nlohmann::json j;
  j["content"] = text;
  const std::string content = j.dump();
  Database::Message msg = MakeMessage(content, "tool_call", tool_call_id, parsing_strategy);
  MessageContext ctx(msg);
  const std::string extracted = MessageParser::ExtractAssistantText(ctx);
  if (ctx.is_valid()) {
    EXPECT_EQ(extracted, text);
  } else {
    EXPECT_TRUE(extracted.empty());
  }
}

void OpenAiFunctionCallsExtracted(const std::string& name, const std::string& id, int arg_value) {
  nlohmann::json call;
  call["id"] = id;
  call["function"] = nlohmann::json{{"name", name}, {"arguments", nlohmann::json{{"value", arg_value}}}};
  nlohmann::json body;
  body["tool_calls"] = nlohmann::json::array({call});
  const std::string content = body.dump();
  Database::Message msg = MakeMessage(content, "tool_call", "fallback", "openai");
  MessageContext ctx(msg);
  auto calls_or = MessageParser::ExtractToolCalls(ctx);
  ASSERT_TRUE(calls_or.ok());
  ASSERT_EQ(calls_or->size(), 1);
  EXPECT_EQ((*calls_or)[0].id, id);
  EXPECT_EQ((*calls_or)[0].name, name);
  ASSERT_TRUE((*calls_or)[0].args.is_object());
  EXPECT_EQ((*calls_or)[0].args.value("value", 0), arg_value);
}

FUZZ_TEST(MessageParserFuzzTest, ExtractToolCallsIsTotal);

FUZZ_TEST(MessageParserFuzzTest, NonToolCallAssistantTextPassesThrough)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::ElementOf<std::string>({"openai", "unknown", ""}));

FUZZ_TEST(MessageParserFuzzTest, ToolCallAssistantTextMatchesJsonContent)
    .WithDomains(fuzztest::InRegexp("[\\x20-\\x7E]{0,128}"),
                 fuzztest::ElementOf<std::string>({"openai", "unknown", ""}),
                 fuzztest::InRegexp("[\\x20-\\x7E]{0,128}"));

FUZZ_TEST(MessageParserFuzzTest, OpenAiFunctionCallsExtracted)
    .WithDomains(fuzztest::InRegexp("[A-Za-z0-9_\\-]{1,32}"), fuzztest::InRegexp("[A-Za-z0-9_\\-]{1,32}"),
                 fuzztest::Arbitrary<int>());

}  // namespace
}  // namespace slop