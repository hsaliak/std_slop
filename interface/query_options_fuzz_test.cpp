
#include "interface/interaction_engine.h"

#include <string>
#include <tuple>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace slop {
namespace {

using Scope = InteractionEngine::QueryOptions::ExecutionScope;

void NormalizeQueryOptionsNeverCrashes(const std::string& session_id, bool has_skill, const std::string& skill,
                                       bool has_context_window, int context_window, int scope_raw, int depth) {
  InteractionEngine::QueryOptions options;
  options.session_id = session_id;
  if (has_skill) options.skill = skill;
  if (has_context_window) options.context_window = context_window;
  options.execution_scope = (scope_raw % 2 == 0) ? Scope::kRoot : Scope::kSubquery;
  options.execution_depth = depth;

  auto normalized = InteractionEngine::NormalizeQueryOptions(options);
  if (!normalized.ok()) {
    EXPECT_FALSE(normalized.ok());
    return;
  }

  EXPECT_FALSE(normalized->session_id.empty());
  if (normalized->skill.has_value()) {
    EXPECT_FALSE(normalized->skill->empty());
  }
  if (normalized->context_window.has_value()) {
    EXPECT_GE(*normalized->context_window, 0);
  }
  EXPECT_TRUE(InteractionEngine::IsValidQueryExecutionContext(*normalized));
  EXPECT_NE(normalized->cancellation, nullptr);
}

void NormalizeQueryOptionsDeterministic(const std::string& session_id, const std::string& skill, int context_window,
                                        bool use_skill, bool use_context, bool subquery, int depth) {
  InteractionEngine::QueryOptions options;
  options.session_id = session_id;
  if (use_skill) options.skill = skill;
  if (use_context) options.context_window = context_window;
  options.execution_scope = subquery ? Scope::kSubquery : Scope::kRoot;
  options.execution_depth = depth;

  auto first = InteractionEngine::NormalizeQueryOptions(options);
  auto second = InteractionEngine::NormalizeQueryOptions(options);

  EXPECT_EQ(first.ok(), second.ok());
  if (!first.ok()) {
    EXPECT_EQ(first.status().code(), second.status().code());
    return;
  }
  EXPECT_EQ(first->session_id, second->session_id);
  EXPECT_EQ(first->skill, second->skill);
  EXPECT_EQ(first->context_window, second->context_window);
  EXPECT_EQ(first->execution_scope, second->execution_scope);
  EXPECT_EQ(first->execution_depth, second->execution_depth);
  EXPECT_NE(first->cancellation, nullptr);
}

FUZZ_TEST(QueryOptionsFuzzTest, NormalizeQueryOptionsNeverCrashes)
    .WithSeeds(std::vector<std::tuple<std::string, bool, std::string, bool, int, int, int>>{
        std::make_tuple(std::string(""), false, std::string(""), false, 0, 0, 0),
        std::make_tuple(std::string("query"), true, std::string("code_reviewer"), true, 20, 1, 1),
        std::make_tuple(std::string("code_review"), true, std::string(""), true, 0, 1, 1),
        std::make_tuple(std::string("code_review"), false, std::string(""), true, 0, 1, 1),
        std::make_tuple(std::string("root"), false, std::string(""), false, 0, 1, 0),
    });

FUZZ_TEST(QueryOptionsFuzzTest, NormalizeQueryOptionsDeterministic)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<std::string>(),
                 fuzztest::InRange(-16, 64),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange(-4, 8));

}  // namespace
}  // namespace slop