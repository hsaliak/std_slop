
#include "tools/common.h"

#include <string>

#include "absl/status/status.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

void ValidateQueryDbArgsDeterministic(const std::string& raw_args) {
  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = nlohmann::json::object();
  }

  const absl::Status first = ValidateQueryDbArgs(args);
  const absl::Status second = ValidateQueryDbArgs(args);
  EXPECT_EQ(first.ok(), second.ok());
  EXPECT_EQ(first.code(), second.code());
}

void QueryDbRequiresSqlString(const std::string& sql_like, bool use_string) {
  nlohmann::json args = nlohmann::json::object();
  if (use_string) {
    args["sql"] = sql_like;
  } else {
    args["sql"] = nlohmann::json::object();
  }

  const absl::Status status = ValidateQueryDbArgs(args);
  EXPECT_EQ(status.ok(), use_string);
}

void QueryDbParamsMustBeArray(const std::string& sql, const std::string& params_raw) {
  nlohmann::json args = nlohmann::json::object();
  args["sql"] = sql;

  nlohmann::json maybe_params = nlohmann::json::parse(params_raw, nullptr, false);
  if (maybe_params.is_discarded()) {
    maybe_params = params_raw;
  }
  args["params"] = maybe_params;

  const absl::Status status = ValidateQueryDbArgs(args);
  if (!maybe_params.is_array()) {
    EXPECT_FALSE(status.ok());
  }
}

FUZZ_TEST(DbToolArgsFuzzTest, ValidateQueryDbArgsDeterministic);

FUZZ_TEST(DbToolArgsFuzzTest, QueryDbRequiresSqlString)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<bool>());

FUZZ_TEST(DbToolArgsFuzzTest, QueryDbParamsMustBeArray)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>());

}  // namespace
}  // namespace slop