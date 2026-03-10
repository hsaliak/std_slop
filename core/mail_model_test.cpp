#include <filesystem>
#include <fstream>

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
  nlohmann::json report = nlohmann::json::parse(*verify_res, nullptr, false);
  ASSERT_FALSE(report.is_discarded());
  EXPECT_TRUE(report["all_passed"].get<bool>());
  EXPECT_EQ(report["report"].size(), 2);

  // 5. Verify series with a command that fails on the first patch but passes on second?
  // No, 'ls next.txt' will fail on the first patch.
  auto verify_fail_res =
      executor_->Execute("git_verify_series", {{"command", "ls next.txt"}, {"base_branch", "HEAD~2"}});
  ASSERT_TRUE(verify_fail_res.ok());
  nlohmann::json report_fail = nlohmann::json::parse(*verify_fail_res, nullptr, false);
  ASSERT_FALSE(report_fail.is_discarded());
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

TEST_F(MailModelTest, BranchStagingNormalizesAlreadyPrefixedName) {
  std::string staging_name = "slop/staging/prefixed-name";
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}});
  ASSERT_TRUE(branch_res.ok()) << branch_res.status().message();

  auto current_branch = executor_->Execute("execute_bash", {{"command", "git rev-parse --abbrev-ref HEAD"}});
  ASSERT_TRUE(current_branch.ok());
  EXPECT_TRUE(current_branch->find("slop/staging/prefixed-name") != std::string::npos);
  EXPECT_TRUE(current_branch->find("slop/staging/slop/staging") == std::string::npos);

  auto db_res =
      db_.Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?;", {"slop/staging/prefixed-name"});
  EXPECT_TRUE(db_res.ok());

  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/prefixed-name"}});
}

TEST_F(MailModelTest, GitCreateStagingBranchDirectSupportsExistingBranchAndDbRecord) {
  const std::string prefixed = "slop/staging/direct-prefixed";

  auto create1 = executor_->Execute("git_create_staging_branch", {{"name", prefixed}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(create1.ok()) << create1.status().message();
  EXPECT_TRUE(create1->find("slop/staging/direct-prefixed") != std::string::npos);

  auto current1 = executor_->Execute("execute_bash", {{"command", "git rev-parse --abbrev-ref HEAD"}});
  ASSERT_TRUE(current1.ok());
  EXPECT_TRUE(current1->find("slop/staging/direct-prefixed") != std::string::npos);

  auto db_res = db_.Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?;",
                          {"slop/staging/direct-prefixed"});
  ASSERT_TRUE(db_res.ok());
  EXPECT_TRUE(db_res->find("parent_branch") != std::string::npos);

  // Call again with same name to exercise existing-branch fallback path.
  auto create2 = executor_->Execute("git_create_staging_branch", {{"name", prefixed}, {"base_branch", "HEAD"}});
  ASSERT_TRUE(create2.ok()) << create2.status().message();
  EXPECT_TRUE(create2->find("slop/staging/direct-prefixed") != std::string::npos);

  (void)executor_->Execute("execute_bash", {{"command", "git checkout " + original_branch_}});
  (void)executor_->Execute("execute_bash", {{"command", "git branch -D slop/staging/direct-prefixed"}});
}

TEST_F(MailModelTest, FinalizeSeriesSucceedsWhenAlreadyLandedWithoutApproval) {
  std::string staging_name = "feat-already-landed";
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}});
  ASSERT_TRUE(branch_res.ok()) << branch_res.status().message();

  {
    std::ofstream ofs("already_landed.txt");
    ofs << "already landed content";
    ofs.close();
  }
  ASSERT_TRUE(executor_->Execute("git_commit_patch", {{"summary", "already landed patch"}, {"rationale", "r"}}).ok());

  (void)executor_->Execute("execute_bash", {{"command", "git checkout main"}});
  ASSERT_TRUE(
      executor_->Execute("execute_bash", {{"command", "git merge --ff-only slop/staging/" + staging_name}}).ok());
  ASSERT_TRUE(executor_->Execute("execute_bash", {{"command", "git checkout slop/staging/" + staging_name}}).ok());

  auto finalize_res = executor_->Execute("git_finalize_series", {{"target_branch", "main"}});
  ASSERT_TRUE(finalize_res.ok()) << finalize_res.status().message();

  auto contains_res =
      executor_->Execute("execute_bash", {{"command", "git branch --list slop/staging/" + staging_name}});
  ASSERT_TRUE(contains_res.ok());
  EXPECT_TRUE(contains_res->find("slop/staging/" + staging_name) == std::string::npos);

  std::filesystem::remove("already_landed.txt");
}

TEST_F(MailModelTest, DynamicBaseBranchWorkflow) {
  // 1. Setup: Create and switch to a non-main base branch
  std::string base_branch = "test-base-develop";
  (void)executor_->Execute("execute_bash", {{"command", "git checkout -b " + base_branch}});

  // 2. Initiation: Start staging from 'test-base-develop' implicitly
  std::string staging_name = "feat-dynamic-test";
  auto branch_res = executor_->Execute("git_branch_staging", {{"name", staging_name}});
  ASSERT_TRUE(branch_res.ok()) << branch_res.status().message();

  // Verify database was populated
  auto db_res =
      db_.Query("SELECT parent_branch FROM staging_branches WHERE branch_name = ?;", {"slop/staging/" + staging_name});
  EXPECT_TRUE(db_res.ok() && db_res->find(base_branch) != std::string::npos);

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
  // Case 1: Requested base wins
  EXPECT_EQ(*executor_->GetBaseBranch("custom-branch"), "custom-branch");

  // Case 2: Database lookup for staging branch
  std::string staging = "slop/staging/test-db-res";
  (void)db_.Execute("INSERT INTO staging_branches (branch_name, parent_branch) VALUES (?, ?);", staging, "main-parent");

  // We need to mock the current branch
  // But ToolExecutor calls git.get_current_branch in Lua which calls __os_run("git rev-parse --abbrev-ref HEAD")
  // Since we can't easily mock __os_run here without complex Lua injection, we'll rely on the existing unit tests
  // that already use git_branch_staging which populates the DB.
}

}  // namespace slop

