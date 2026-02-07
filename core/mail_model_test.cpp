#include <filesystem>
#include <fstream>

#include "absl/strings/match.h"

#include "core/tool_executor.h"

#include <gtest/gtest.h>

namespace slop {

class MailModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(db_.Init(":memory:").ok());
    executor_ = *ToolExecutor::Create(&db_);

    // Ensure we are in a git repo for testing, or skip
    auto res = executor_->Execute("execute_bash", {{"command", "git rev-parse --is-inside-work-tree"}});
    if (!res.ok() || res->find("true") == std::string::npos) {
      GTEST_SKIP() << "Not in a git repository, skipping MailModelTest";
    }

    // Get current branch to restore later
    auto branch_res = executor_->Execute("execute_bash", {{"command", "git rev-parse --abbrev-ref HEAD"}});
    original_branch_ = *branch_res;
    // Strip TOOL_RESULT header if present (though execute_bash doesn't add it in internal call usually)
    if (absl::StrContains(original_branch_, "### TOOL_RESULT")) {
      // simplified
    }
    // Actually, let's just use a temporary directory for git tests if possible,
    // but for now we'll just be careful.
  }

  void TearDown() override {
    // Restore original branch if we changed it
    // system("git checkout " + original_branch_);
  }

  Database db_;
  std::unique_ptr<ToolExecutor> executor_;
  std::string original_branch_;
};

TEST_F(MailModelTest, Phase1Tools) {
  std::string staging_name = "test-phase-1-tool";

  // 1. GitBranchStaging
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(branch_res.ok()) << branch_res.status().message();
  EXPECT_TRUE(branch_res->find("Created and checked out staging branch") != std::string::npos);

  // 2. GitCommitPatch
  // Create a dummy file
  std::ofstream ofs("dummy_patch.txt");
  ofs << "dummy content";
  ofs.close();

  auto commit_res = executor_->Execute(
      "git_commit_patch",
      {{"summary", "test: add dummy patch"}, {"rationale", "Testing the patch commit tool functionality."}});
  ASSERT_TRUE(commit_res.ok()) << commit_res.status().message();
  EXPECT_TRUE(commit_res->find("Committed patch") != std::string::npos);

  // 3. GitFormatPatchSeries
  auto format_res = executor_->Execute("git_format_patch_series", {{"base_branch", "HEAD~1"}});
  ASSERT_TRUE(format_res.ok()) << format_res.status().message();
  EXPECT_TRUE(format_res->find("Rationale: Testing the patch commit tool functionality.") != std::string::npos);

  // 4. GitFinalizeSeries
  // Note: This will merge back to HEAD~1 or similar, but since we are on a new branch,
  // let's just check it doesn't crash and returns something reasonable.
  // Actually, finalizing might be destructive to our test state, so we'll be careful.

  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("dummy_patch.txt");
}

TEST_F(MailModelTest, VerifySeries) {
  std::string staging_name = "test-verify-series";

  // 1. Start staging
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(branch_res.ok());

  // 2. Add a good patch
  std::ofstream ofs1("good.txt");
  ofs1 << "good";
  ofs1.close();
  auto commit1 = executor_->Execute("git_commit_patch", {{"summary", "good patch"}, {"rationale", "rationale"}});
  ASSERT_TRUE(commit1.ok());

  // 3. Add another patch
  std::ofstream ofs2("next.txt");
  ofs2 << "next";
  ofs2.close();
  auto commit2 = executor_->Execute("git_commit_patch", {{"summary", "next patch"}, {"rationale", "rationale"}});
  ASSERT_TRUE(commit2.ok());

  // 4. Verify series with a command that passes
  auto verify_res = executor_->Execute("git_verify_series", {{"command", "ls good.txt"}, {"base_branch", "HEAD~2"}});
  ASSERT_TRUE(verify_res.ok()) << verify_res.status().message();
  nlohmann::json report = nlohmann::json::parse(*verify_res);
  EXPECT_TRUE(report["all_passed"].get<bool>());
  EXPECT_EQ(report["report"].size(), 2);

  // 5. Verify series with a command that fails on the first patch but passes on second?
  // No, 'ls next.txt' will fail on the first patch.
  auto verify_fail_res =
      executor_->Execute("git_verify_series", {{"command", "ls next.txt"}, {"base_branch", "HEAD~2"}});
  ASSERT_TRUE(verify_fail_res.ok());
  nlohmann::json report_fail = nlohmann::json::parse(*verify_fail_res);
  EXPECT_FALSE(report_fail["all_passed"].get<bool>());
  EXPECT_EQ(report_fail["report"][0]["status"], "failed");  // next.txt doesn't exist in first patch
  EXPECT_EQ(report_fail["report"][1]["status"], "passed");  // next.txt exists in second patch

  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("good.txt");
  std::filesystem::remove("next.txt");
}

TEST_F(MailModelTest, RerollPatch) {
  std::string staging_name = "test-reroll-patch";

  // 1. Start staging
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(branch_res.ok());

  // 2. Add patch 1
  {
    std::ofstream ofs("file1.txt");
    ofs << "v1";
    ofs.close();
    auto res = executor_->Execute("git_commit_patch", {{"summary", "p1"}, {"rationale", "r1"}});
    ASSERT_TRUE(res.ok());
  }

  // 3. Add patch 2
  {
    std::ofstream ofs("file2.txt");
    ofs << "v1";
    ofs.close();
    auto res = executor_->Execute("git_commit_patch", {{"summary", "p2"}, {"rationale", "r2"}});
    ASSERT_TRUE(res.ok());
  }

  // 4. Modify file1.txt (which belongs to patch 1)
  {
    std::ofstream ofs("file1.txt");
    ofs << "v2";
    ofs.close();
  }

  // 5. Reroll into patch 1
  auto reroll_res = executor_->Execute("git_reroll_patch", {{"index", 1}, {"base_branch", "HEAD~2"}});
  ASSERT_TRUE(reroll_res.ok()) << reroll_res.status().message();
  EXPECT_TRUE(reroll_res->find("Successfully rerolled") != std::string::npos);

  // 6. Verify the series still has 2 patches and file1.txt is v2 in both,
  // but specifically check that the change is now part of the first commit.

  // Check that we still have exactly 2 commits since HEAD~2
  auto log_res = executor_->Execute("execute_bash", {{"command", "git rev-list HEAD~2..HEAD | wc -l"}});
  // Note: wc -l might have spaces
  EXPECT_TRUE(log_res->find("2") != std::string::npos);

  // Check content of file1.txt at patch 1
  auto content_res = executor_->Execute("execute_bash", {{"command", "git show HEAD~1:file1.txt"}});
  EXPECT_TRUE(content_res->find("v2") != std::string::npos);

  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("file1.txt");
  std::filesystem::remove("file2.txt");
}

TEST_F(MailModelTest, FormatPatchSeries) {
  std::string staging_name = "test-format-series";

  // 1. Start staging
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(branch_res.ok());

  // 2. Add patch
  std::ofstream ofs("test.txt");
  ofs << "content";
  ofs.close();
  auto commit_res =
      executor_->Execute("git_commit_patch", {{"summary", "test summary"}, {"rationale", "test rationale"}});
  ASSERT_TRUE(commit_res.ok());

  // 3. Format series
  auto format_res = executor_->Execute("git_format_patch_series", {{"base_branch", "HEAD~1"}});
  ASSERT_TRUE(format_res.ok());

  EXPECT_TRUE(format_res->find("### Patch [1/1]: test summary ###") != std::string::npos);
  EXPECT_TRUE(format_res->find("test rationale") != std::string::npos);
  EXPECT_TRUE(format_res->find("+content") != std::string::npos);

  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("test.txt");
}

TEST_F(MailModelTest, DynamicBaseBranchWorkflow) {
  // 1. Setup: Create and switch to a non-main base branch
  std::string base_branch = "test-base-develop";
  (void)executor_->Execute("execute_bash", {{"command", "git checkout -b " + base_branch}});

  // 2. Initiation: Start staging from 'test-base-develop' implicitly
  std::string staging_name = "feat-dynamic-test";
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}});
  ASSERT_TRUE(branch_res.ok()) << branch_res.status().message();

  // Verify config was set
  auto config_res = executor_->Execute("execute_bash", {{"command", "git config slop.basebranch"}});
  EXPECT_TRUE(config_res->find(base_branch) != std::string::npos);

  // 3. Work: Add a patch
  {
    std::ofstream ofs("feature.txt");
    ofs << "new feature";
    ofs.close();
  }
  ASSERT_TRUE(executor_->Execute("git_commit_patch", {{"summary", "add feature"}, {"rationale", "req"}}).ok());

  // 4. Implicit Format: Should use 'test-base-develop' from config
  auto format_res = executor_->Execute("git_format_patch_series", {});
  ASSERT_TRUE(format_res.ok());
  EXPECT_TRUE(format_res->find("add feature") != std::string::npos);
  // It should NOT show "range main..HEAD" if it correctly used the dynamic base
  // However, git_format_patch_series output doesn't explicitly print the range in the text,
  // it just uses it to generate the patches.
  // But we can check if it found the patch.

  // 5. Finalization: Should merge back to 'test-base-develop' and cleanup
  auto finalize_res = executor_->Execute("git_finalize_series", {});
  ASSERT_TRUE(finalize_res.ok());

  // Verify we are back on base_branch and config is gone
  auto current_branch = executor_->Execute("execute_bash", {{"command", "git rev-parse --abbrev-ref HEAD"}});
  EXPECT_TRUE(current_branch->find(base_branch) != std::string::npos);

  // Verify content was merged and exists on the base branch
  EXPECT_TRUE(std::filesystem::exists("feature.txt"));

  // Verify the slop.basebranch configuration was cleaned up
  auto cleanup_res = executor_->Execute("execute_bash", {{"command", "git config slop.basebranch"}});
  // Git returns non-zero when config is not found.
  EXPECT_TRUE(cleanup_res->find("exit_code: 1") != std::string::npos || cleanup_res->empty());

  // Cleanup repo
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D " + base_branch}});
  // staging branch should already be deleted by git_finalize_series, but we ensure it for robustness
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("feature.txt");
}

TEST_F(MailModelTest, VerifySeriesDynamicBase) {
  std::string base_branch = "test-verify-base";
  (void)executor_->Execute("execute_bash", {{"command", "git checkout -b " + base_branch}});

  std::string staging_name = "feat-verify-test";
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}});
  ASSERT_TRUE(branch_res.ok());

  // Create a commit
  {
    std::ofstream ofs("verify_me.txt");
    ofs << "content";
    ofs.close();
  }
  ASSERT_TRUE(executor_->Execute("git_commit_patch", {{"summary", "test verify"}, {"rationale", "r"}}).ok());

  // Verify should work implicitly using the base branch from config
  // We use a simple command that succeeds if the file is present
  auto verify_res = executor_->Execute("git_verify_series", {{"command", "ls verify_me.txt"}});
  EXPECT_TRUE(verify_res.ok()) << verify_res.status().message();

  // Cleanup
  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D " + base_branch}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/" + staging_name}});
  std::filesystem::remove("verify_me.txt");
}

TEST_F(MailModelTest, GetBaseBranchResolution) {
  // Priority 1: Requested base
  EXPECT_EQ(executor_->GetBaseBranch("custom-branch"), "custom-branch");

  // Priority 2: Config slop.basebranch
  (void)executor_->Execute("execute_bash", {{"command", "git config slop.basebranch config-branch"}});
  EXPECT_EQ(executor_->GetBaseBranch(""), "config-branch");

  // Priority 1 still wins over Config
  EXPECT_EQ(executor_->GetBaseBranch("explicit-wins"), "explicit-wins");

  // Clear config
  (void)executor_->Execute("execute_bash", {{"command", "git config --unset slop.basebranch"}});

  // Priority 3: Fallback detection
  // This is harder to test without messing with 'main'/'master' existence,
  // but we can at least verify it returns 'main' if it exists.
  auto main_exists = executor_->Execute("execute_bash", {{"command", "git rev-parse --verify main"}});
  if (main_exists.ok()) {
    EXPECT_EQ(executor_->GetBaseBranch(""), "main");
  }
}

}  // namespace slop
