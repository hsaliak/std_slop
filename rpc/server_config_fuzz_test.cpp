#include "rpc/server_config.h"

#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "google/protobuf/text_format.h"

namespace slop::rpc::v1 {
namespace {

void LoadServerConfigTextprotoNeverCrashes(const std::string& textproto) {
  ServerConfig config;
  if (!google::protobuf::TextFormat::ParseFromString(textproto, &config)) {
    return;
  }

  auto runtime_or = BuildServerRuntimeConfig(config);
  if (runtime_or.ok()) {
    EXPECT_FALSE(runtime_or->listen_addr.empty());
    EXPECT_FALSE(runtime_or->db_path.empty());
    EXPECT_FALSE(runtime_or->runtime_options.model.empty());
    EXPECT_TRUE(runtime_or->disable_ask_user);
    EXPECT_GE(runtime_or->proto.policy().max_execution_depth(), 0);
  } else {
    EXPECT_FALSE(runtime_or.status().message().empty());
  }
}

FUZZ_TEST(ServerConfigFuzzTest, LoadServerConfigTextprotoNeverCrashes)
    .WithSeeds({std::string(R"pb(
      listen_addr: "127.0.0.1:50051"
      db_path: ":memory:"
      provider { provider: "gemini" model: "gemini-test" gemini_api_key_env: "GEMINI_API_KEY" }
      policy { disable_ask_user: true max_execution_depth: 1 }
      defaults { context_window: 4 }
    )pb"),
                std::string(R"pb(
      listen_addr: "127.0.0.1:50051"
      db_path: ":memory:"
      provider { provider: "openai" model: "gpt-test" openai_api_key_env: "OPENAI_API_KEY" }
      policy { disable_ask_user: false max_execution_depth: -1 }
    )pb")});

}  // namespace
}  // namespace slop::rpc::v1
