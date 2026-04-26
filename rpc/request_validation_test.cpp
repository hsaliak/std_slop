#include "rpc/request_validation.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"

namespace slop::rpc::v1 {
namespace {

ServerRuntimeConfig BaseConfig() {
  ServerRuntimeConfig config;
  config.runtime_options.model = "base-model";
  config.active_skills = {"code_reviewer"};
  config.max_context_window = 8;
  return config;
}

TEST(RequestValidationTest, RejectsMissingPrompt) {
  RunPromptRequest request;
  request.set_prompt("   ");

  auto validated_or = ValidateRunPromptRequest(request, BaseConfig());

  ASSERT_FALSE(validated_or.ok());
  EXPECT_EQ(validated_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RequestValidationTest, RejectsDisallowedModelOverride) {
  RunPromptRequest request;
  request.set_prompt("hello");
  request.set_model_override("override-model");

  auto validated_or = ValidateRunPromptRequest(request, BaseConfig());

  ASSERT_FALSE(validated_or.ok());
  EXPECT_EQ(validated_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RequestValidationTest, RejectsDisallowedSkillOverride) {
  RunPromptRequest request;
  request.set_prompt("hello");
  request.add_active_skills("planner");

  auto validated_or = ValidateRunPromptRequest(request, BaseConfig());

  ASSERT_FALSE(validated_or.ok());
  EXPECT_EQ(validated_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RequestValidationTest, RejectsDisallowedContextWindowOverride) {
  RunPromptRequest request;
  request.set_prompt("hello");
  request.set_context_window(4);

  auto validated_or = ValidateRunPromptRequest(request, BaseConfig());

  ASSERT_FALSE(validated_or.ok());
  EXPECT_EQ(validated_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RequestValidationTest, MarksServerDefaultSkillsAsNotARequestOverride) {
  RunPromptRequest request;
  request.set_prompt("hello");

  auto validated_or = ValidateRunPromptRequest(request, BaseConfig());

  ASSERT_TRUE(validated_or.ok()) << validated_or.status();
  EXPECT_EQ(validated_or->active_skills, std::vector<std::string>{"code_reviewer"});
  EXPECT_FALSE(validated_or->active_skills_override);
}

TEST(RequestValidationTest, AppliesAllowedOverrides) {
  ServerRuntimeConfig config = BaseConfig();
  config.allow_request_model_override = true;
  config.allow_request_skill_override = true;
  config.allow_request_context_window_override = true;

  RunPromptRequest request;
  request.set_prompt("  hello  ");
  request.set_session_id(" session ");
  request.set_model_override("override-model");
  request.add_active_skills("planner");
  request.set_context_window(4);

  auto validated_or = ValidateRunPromptRequest(request, config);

  ASSERT_TRUE(validated_or.ok()) << validated_or.status();
  EXPECT_EQ(validated_or->prompt, "hello");
  EXPECT_EQ(validated_or->session_id, "session");
  ASSERT_TRUE(validated_or->model_override.has_value());
  EXPECT_EQ(*validated_or->model_override, "override-model");
  EXPECT_EQ(validated_or->active_skills, std::vector<std::string>{"planner"});
  EXPECT_TRUE(validated_or->active_skills_override);
  ASSERT_TRUE(validated_or->context_window.has_value());
  EXPECT_EQ(*validated_or->context_window, 4);
}

TEST(RequestValidationTest, RejectsContextWindowAbovePolicyMaximum) {
  ServerRuntimeConfig config = BaseConfig();
  config.allow_request_context_window_override = true;

  RunPromptRequest request;
  request.set_prompt("hello");
  request.set_context_window(9);

  auto validated_or = ValidateRunPromptRequest(request, config);

  ASSERT_FALSE(validated_or.ok());
  EXPECT_EQ(validated_or.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::rpc::v1
