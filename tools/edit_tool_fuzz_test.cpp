
#include "tools/tool_executor.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

#include "core/database.h"
#include "nlohmann/json.hpp"

namespace slop {
namespace {

constexpr char kEditTargetFile[] = "edit_tool_fuzz_target.txt";
constexpr char kBaselineContent[] = "alpha\nbeta\ngamma\nbeta\n";

void WriteBaselineFile() {
  std::ofstream out(kEditTargetFile, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << kBaselineContent;
  ASSERT_TRUE(out.good());
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

nlohmann::json MakeEditArgs(const std::string& op, const std::string& find, const std::string& text,
                            const std::string& which) {
  nlohmann::json edit = {{"op", op}, {"find", find}};
  if (op != "delete") edit["text"] = text;
  if (!which.empty()) edit["which"] = which;
  return {{"path", kEditTargetFile}, {"edits", nlohmann::json::array({edit})}};
}

void EditToolArgShapeNoCrash(const std::string& raw_args) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::filesystem::remove(kEditTargetFile);
  WriteBaselineFile();
  const std::string before = ReadWholeFile(kEditTargetFile);

  nlohmann::json args = nlohmann::json::parse(raw_args, nullptr, false);
  if (args.is_discarded()) {
    args = raw_args;
  }

  const auto res = executor.Execute("edit_tool", args);
  (void)res;

  if (!res.ok()) {
    EXPECT_EQ(before, ReadWholeFile(kEditTargetFile));
  }

  std::filesystem::remove(kEditTargetFile);
}

void EditToolSingleEditNoCrash(const std::string& op, const std::string& find, const std::string& text,
                               const std::string& which) {
  Database db;
  ASSERT_TRUE(db.Init(":memory:").ok());
  auto executor_or = ToolExecutor::Create(&db);
  ASSERT_TRUE(executor_or.ok());
  auto& executor = **executor_or;

  std::filesystem::remove(kEditTargetFile);
  WriteBaselineFile();
  const std::string before = ReadWholeFile(kEditTargetFile);

  const auto res = executor.Execute("edit_tool", MakeEditArgs(op, find, text, which));
  if (!res.ok()) {
    EXPECT_EQ(before, ReadWholeFile(kEditTargetFile));
  } else {
    EXPECT_FALSE(res->empty());
  }

  std::filesystem::remove(kEditTargetFile);
}

FUZZ_TEST(EditToolFuzzTest, EditToolArgShapeNoCrash).WithDomains(fuzztest::Arbitrary<std::string>());

FUZZ_TEST(EditToolFuzzTest, EditToolSingleEditNoCrash)
    .WithDomains(fuzztest::ElementOf<std::string>({"replace", "insert_before", "insert_after", "delete", "bad"}),
                 fuzztest::Arbitrary<std::string>(), fuzztest::Arbitrary<std::string>(),
                 fuzztest::ElementOf<std::string>({"", "only", "first", "last", "bad"}));

}  // namespace
}  // namespace slop
