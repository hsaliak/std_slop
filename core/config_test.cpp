#include "core/config.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>


#include <gtest/gtest.h>

namespace slop {
namespace {

std::string WriteTempConfig(const std::string& content) {
  char path[] = "/tmp/slop_config_test_XXXXXX.ini";
  int fd = mkstemps(path, 4);
  EXPECT_NE(fd, -1);
  if (fd != -1) {
    close(fd);
  }
  std::ofstream out(path);
  out << content;
  out.close();
  return std::string(path);
}

}  // namespace

TEST(ConfigSpecializationTest, LoadsValidSpecializations) {
  std::string cfg_path = WriteTempConfig(R"ini(
[llm_tool_code_review_llm]
system_prompt_patch = review code carefully
session_id = code_review
skill = code_reviewer
context_window = 8

[llm_tool_explorer_llm]
system_prompt_patch = explore repository
session_id = data_explorer
skill = data_explorer
)ini");

  auto res = LoadLlmToolSpecializations(cfg_path);
  ASSERT_TRUE(res.ok()) << res.status();
  ASSERT_EQ(res->size(), 2u);

  const auto& first = (*res)[0];
  EXPECT_EQ(first.tool_name, "llm_tool_code_review_llm");
  EXPECT_EQ(first.system_prompt_patch, "review code carefully");
  EXPECT_EQ(first.session_id, "code_review");
  EXPECT_EQ(first.skill, "code_reviewer");
  ASSERT_TRUE(first.context_window.has_value());
  EXPECT_EQ(*first.context_window, 8);

  const auto& second = (*res)[1];
  EXPECT_EQ(second.tool_name, "llm_tool_explorer_llm");
  EXPECT_EQ(second.system_prompt_patch, "explore repository");
  EXPECT_EQ(second.session_id, "data_explorer");
  EXPECT_EQ(second.skill, "data_explorer");
  EXPECT_FALSE(second.context_window.has_value());

  std::filesystem::remove(cfg_path);
}

TEST(ConfigSpecializationTest, MissingRequiredKeyRejected) {
  std::string cfg_path = WriteTempConfig(R"ini(
[llm_tool_code_review_llm]
system_prompt_patch = review code carefully
session_id = code_review
)ini");

  auto res = LoadLlmToolSpecializations(cfg_path);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);

  std::filesystem::remove(cfg_path);
}

TEST(ConfigSpecializationTest, ZeroContextWindowAcceptedAsInfinite) {
  std::string cfg_path = WriteTempConfig(R"ini(
[llm_tool_code_review_llm]
system_prompt_patch = review code carefully
session_id = code_review
skill = code_reviewer
context_window = 0
)ini");

  auto res = LoadLlmToolSpecializations(cfg_path);
  ASSERT_TRUE(res.ok()) << res.status();
  ASSERT_EQ(res->size(), 1u);
  ASSERT_TRUE((*res)[0].context_window.has_value());
  EXPECT_EQ(*(*res)[0].context_window, 0);

  std::filesystem::remove(cfg_path);
}

TEST(ConfigSpecializationTest, InvalidContextWindowRejected) {
  std::string cfg_path = WriteTempConfig(R"ini(
[llm_tool_code_review_llm]
system_prompt_patch = review code carefully
session_id = code_review
skill = code_reviewer
context_window = -1
)ini");

  auto res = LoadLlmToolSpecializations(cfg_path);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);
  std::filesystem::remove(cfg_path);
}

TEST(ConfigSpecializationTest, EmptySuffixRejected) {
  std::string cfg_path = WriteTempConfig(R"ini(
[llm_tool_]
system_prompt_patch = review code carefully
session_id = code_review
skill = code_reviewer
)ini");

  auto res = LoadLlmToolSpecializations(cfg_path);
  ASSERT_FALSE(res.ok());
  EXPECT_EQ(res.status().code(), absl::StatusCode::kInvalidArgument);

  std::filesystem::remove(cfg_path);
}

TEST(ConfigSpecializationTest, MissingFileReturnsEmpty) {
  auto res = LoadLlmToolSpecializations("/tmp/does_not_exist_slop_config_1234.ini");
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(res->empty());
}

}  // namespace slop
