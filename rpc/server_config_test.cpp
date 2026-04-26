#include "rpc/server_config.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "absl/status/status.h"
#include "core/config.h"
#include "google/protobuf/text_format.h"

namespace slop::rpc::v1 {
namespace {

ServerConfig ParseConfigOrDie(const std::string& textproto) {
  ServerConfig config;
  CHECK(google::protobuf::TextFormat::ParseFromString(textproto, &config));
  return config;
}

std::string WriteTempConfig(const std::string& content) {
  char path[] = "/tmp/slop_rpc_config_test_XXXXXX";
  const int fd = mkstemp(path);
  CHECK_NE(fd, -1);
  FILE* file = fdopen(fd, "w");
  CHECK_NE(file, nullptr);
  CHECK_EQ(fwrite(content.data(), 1, content.size(), file), content.size());
  CHECK_EQ(fclose(file), 0);
  return path;
}

TEST(ServerConfigTest, ParsesDocsServerConfigAndBuildsRuntimeOptions) {
  auto runtime_or = LoadServerRuntimeConfig("docs/impl/rpc/server.cfg");

  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();
  EXPECT_EQ(runtime_or->listen_addr, "0.0.0.0:50051");
  EXPECT_EQ(runtime_or->db_path, "slop_rpc.db");
  EXPECT_EQ(runtime_or->runtime_options.model, "gemini-2.5-pro");
  EXPECT_FALSE(runtime_or->runtime_options.openai_oauth);
  EXPECT_FALSE(runtime_or->runtime_options.use_responses);
  EXPECT_EQ(runtime_or->active_skills, std::vector<std::string>{"code_reviewer"});
  ASSERT_TRUE(runtime_or->context_window.has_value());
  EXPECT_EQ(*runtime_or->context_window, 12);
  EXPECT_TRUE(runtime_or->allow_request_model_override);
  EXPECT_TRUE(runtime_or->allow_request_skill_override);
  EXPECT_TRUE(runtime_or->allow_request_context_window_override);
  ASSERT_EQ(runtime_or->runtime_options.llm_specializations.size(), 2);
  EXPECT_EQ(runtime_or->runtime_options.llm_specializations[0].tool_name, "llm_tool_code_review_llm");
  EXPECT_EQ(runtime_or->runtime_options.llm_specializations[0].session_id, "code_review");
  EXPECT_EQ(runtime_or->runtime_options.llm_specializations[0].skill, "code_reviewer");
  ASSERT_TRUE(runtime_or->runtime_options.llm_specializations[0].context_window.has_value());
  EXPECT_EQ(*runtime_or->runtime_options.llm_specializations[0].context_window, 8);
}

TEST(ServerConfigTest, ParsesDocsInMemoryConfig) {
  auto runtime_or = LoadServerRuntimeConfig("docs/impl/rpc/server_in_memory.cfg");

  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();
  EXPECT_EQ(runtime_or->listen_addr, "127.0.0.1:50052");
  EXPECT_EQ(runtime_or->db_path, ":memory:");
  EXPECT_EQ(runtime_or->runtime_options.model, "gpt-5.3-codex");
  EXPECT_TRUE(runtime_or->runtime_options.openai_oauth);
  ASSERT_TRUE(runtime_or->context_window.has_value());
  EXPECT_EQ(*runtime_or->context_window, 6);
  EXPECT_FALSE(runtime_or->allow_request_model_override);
  EXPECT_FALSE(runtime_or->allow_request_skill_override);
  EXPECT_FALSE(runtime_or->allow_request_context_window_override);
  ASSERT_EQ(runtime_or->runtime_options.llm_specializations.size(), 1);
  EXPECT_EQ(runtime_or->runtime_options.llm_specializations[0].tool_name, "llm_tool_code_review_llm");
  ASSERT_TRUE(runtime_or->runtime_options.llm_specializations[0].context_window.has_value());
  EXPECT_EQ(*runtime_or->runtime_options.llm_specializations[0].context_window, 4);
}

TEST(ServerConfigTest, ConvertsSpecializationsWithIniParity) {
  const ServerConfig config = ParseConfigOrDie(R"pb(
    listen_addr: "127.0.0.1:50051"
    db_path: ":memory:"
    provider { provider: "gemini" model: "gemini-test" gemini_api_key_env: "GEMINI_API_KEY" }
    policy { disable_ask_user: true }
    llm_tool_specializations {
      name: "explorer_llm"
      system_prompt_patch: "You explore repository structure and summarize findings with exact file paths."
      session_id: "data_explorer"
      skill: "data_explorer"
      context_window: 0
    }
  )pb");

  auto runtime_or = BuildServerRuntimeConfig(config);

  ASSERT_TRUE(runtime_or.ok()) << runtime_or.status();
  ASSERT_EQ(runtime_or->runtime_options.llm_specializations.size(), 1);
  const slop::LlmToolSpecializationConfig& specialization = runtime_or->runtime_options.llm_specializations[0];
  EXPECT_EQ(specialization.tool_name, "llm_tool_explorer_llm");
  EXPECT_EQ(specialization.system_prompt_patch,
            "You explore repository structure and summarize findings with exact file paths.");
  EXPECT_EQ(specialization.session_id, "data_explorer");
  EXPECT_EQ(specialization.skill, "data_explorer");
  EXPECT_FALSE(specialization.context_window.has_value());
}

TEST(ServerConfigTest, RejectsInteractivePolicy) {
  const ServerConfig config = ParseConfigOrDie(R"pb(
    listen_addr: "127.0.0.1:50051"
    db_path: ":memory:"
    provider { provider: "gemini" model: "gemini-test" gemini_api_key_env: "GEMINI_API_KEY" }
    policy { disable_ask_user: false }
  )pb");

  absl::Status status = ValidateServerConfig(config);

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(ServerConfigTest, RejectsDefaultContextWindowAbovePolicyMaximum) {
  const ServerConfig config = ParseConfigOrDie(R"pb(
    listen_addr: "127.0.0.1:50051"
    db_path: ":memory:"
    provider { provider: "gemini" model: "gemini-test" gemini_api_key_env: "GEMINI_API_KEY" }
    policy { disable_ask_user: true max_context_window: 4 }
    defaults { context_window: 8 }
  )pb");

  absl::Status status = ValidateServerConfig(config);

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(ServerConfigTest, RejectsMalformedTextproto) {
  const std::string path = WriteTempConfig("not a textproto: {");

  auto config_or = LoadServerConfigTextproto(path);

  ASSERT_FALSE(config_or.ok());
  EXPECT_EQ(config_or.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace slop::rpc::v1
