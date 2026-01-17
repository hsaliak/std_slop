#include "git_helper.h"
#include <cstdio>
#include <memory>
#include <array>
#include <sstream>
#include <iostream>
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"

namespace slop {

namespace {
std::string Exec(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

int ExecStatus(const std::string& cmd) {
    return system((cmd + " > /dev/null 2>&1").c_str());
}
}

bool GitHelper::IsGitRepo() {
    return ExecStatus("git rev-parse --is-inside-work-tree") == 0;
}

absl::Status GitHelper::InitRepo() {
    if (ExecStatus("git init") == 0) return absl::OkStatus();
    return absl::InternalError("Failed to initialize git repository");
}

absl::StatusOr<bool> GitHelper::HasChanges() {
    std::string out = Exec("git status --porcelain");
    return !absl::StripAsciiWhitespace(out).empty();
}

absl::Status GitHelper::CommitGroup(const std::string& group_id, const std::string& prompt) {
    if (ExecStatus("git add .") != 0) return absl::InternalError("git add failed");
    
    // Sanitize prompt for commit message: remove quotes and newlines
    std::string sanitized_prompt = prompt;
    for (char &c : sanitized_prompt) {
        if (c == '"' || c == '\'' || c == '\n' || c == '\r') c = ' ';
    }

    std::string cmd = absl::StrFormat("git commit -m \"vibe_id: %s\" -m \"Prompt: %s\"", group_id, sanitized_prompt);
    if (ExecStatus(cmd) != 0) return absl::InternalError("git commit failed");
    
    return absl::OkStatus();
}

absl::StatusOr<std::string> GitHelper::GetHashForGroupId(const std::string& group_id) {
    std::string cmd = absl::StrFormat("git log --grep=\"vibe_id: %s\" --format=\"%%H\" -n 1", group_id);
    std::string hash = std::string(absl::StripAsciiWhitespace(Exec(cmd)));
    if (hash.empty()) return absl::NotFoundError("No commit found for group_id: " + group_id);
    return hash;
}

absl::Status GitHelper::UndoCommit(const std::string& group_id) {
    auto hash_or = GetHashForGroupId(group_id);
    if (!hash_or.ok()) return hash_or.status();

    // Check if it's the latest commit
    std::string latest = std::string(absl::StripAsciiWhitespace(Exec("git rev-parse HEAD")));
    if (latest == *hash_or) {
        if (ExecStatus("git reset --hard HEAD~1") == 0) return absl::OkStatus();
    } else {
        // Surgical revert
        if (ExecStatus("git revert " + *hash_or + " --no-edit") == 0) return absl::OkStatus();
    }
    return absl::InternalError("Failed to undo commit");
}

absl::StatusOr<std::string> GitHelper::GetDiff(const std::string& group_id) {
    auto hash_or = GetHashForGroupId(group_id);
    if (!hash_or.ok()) return hash_or.status();
    
    std::string diff = Exec("git show " + *hash_or);
    return diff;
}

}  // namespace slop
