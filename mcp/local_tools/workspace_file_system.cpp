#include "mcp/local_tools/workspace_file_system.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "core/status_macros.h"
#include "re2/re2.h"

namespace slop::mcp::local_tools {
namespace {

constexpr std::size_t kMaxReadBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxWriteBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxGrepBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxGrepFiles = 10000;
constexpr std::size_t kMaxPatternBytes = 4096;
constexpr std::size_t kMaxOutputBytes = 1024 * 1024;
constexpr std::size_t kMaxDirectoryEntries = 1000;
constexpr int kMaxGrepDepth = 32;

const std::vector<std::string>& DefaultIgnorePatterns() {
  static const std::vector<std::string> patterns = {".git", "node_modules", "bazel-*", "dist", "build",
                                                     ".cache", ".next", "target"};
  return patterns;
}

absl::Status FileError(absl::string_view operation, absl::string_view path, int error_number) {
  const std::string message = absl::StrCat(operation, " '", path, "': ", std::strerror(error_number));
  switch (error_number) {
    case ENOENT:
      return absl::NotFoundError(message);
    case EACCES:
    case EPERM:
    case ELOOP:
      return absl::PermissionDeniedError(message);
    case ENOTDIR:
      return absl::InvalidArgumentError(message);
    default:
      return absl::UnavailableError(message);
  }
}

bool IsWindowsAbsolutePath(absl::string_view path) {
  if (path.size() >= 2 && absl::ascii_isalpha(path[0]) && path[1] == ':') return true;
  return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
}

absl::StatusOr<int> OpenDirectory(const std::filesystem::path& root, const std::vector<std::string>& components) {
  int descriptor = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return FileError("Cannot open workspace", root.string(), errno);
  absl::Cleanup close_descriptor = [&descriptor] {
    if (descriptor >= 0) close(descriptor);
  };

  for (const std::string& component : components) {
    const int next_descriptor =
        openat(descriptor, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next_descriptor < 0) {
      const int error_number = errno;
      if (error_number == ENOTDIR || error_number == ELOOP) {
        struct stat info {};
        if (fstatat(descriptor, component.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode)) {
          return absl::PermissionDeniedError(absl::StrCat("Symlink path component is not allowed: ", component));
        }
      }
      return FileError("Cannot open directory", component, error_number);
    }
    close(descriptor);
    descriptor = next_descriptor;
  }

  const int result = descriptor;
  descriptor = -1;
  return result;
}

absl::StatusOr<int> OpenParentDirectory(const std::filesystem::path& root,
                                       const std::vector<std::string>& components) {
  if (components.empty()) return absl::InvalidArgumentError("A file path is required");
  const std::vector<std::string> parent_components(components.begin(), components.end() - 1);
  return OpenDirectory(root, parent_components);
}

absl::StatusOr<std::string> ReadFileAt(int parent_descriptor, absl::string_view filename,
                                      std::size_t max_bytes) {
  const std::string name(filename);
  const int descriptor = openat(parent_descriptor, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return FileError("Cannot open file", filename, errno);
  absl::Cleanup close_descriptor = [descriptor] { close(descriptor); };

  struct stat info {};
  if (fstat(descriptor, &info) != 0) return FileError("Cannot inspect file", filename, errno);
  if (!S_ISREG(info.st_mode)) return absl::InvalidArgumentError(absl::StrCat("Not a regular file: ", filename));
  if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    return absl::ResourceExhaustedError(absl::StrCat("File exceeds the ", max_bytes, " byte limit: ", filename));
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(info.st_size));
  char buffer[16 * 1024];
  while (true) {
    const ssize_t bytes_read = read(descriptor, buffer, sizeof(buffer));
    if (bytes_read == 0) break;
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      return FileError("Cannot read file", filename, errno);
    }
    const std::size_t count = static_cast<std::size_t>(bytes_read);
    if (count > max_bytes - content.size()) {
      return absl::ResourceExhaustedError(absl::StrCat("File exceeds the ", max_bytes, " byte limit: ", filename));
    }
    content.append(buffer, count);
  }
  return content;
}

absl::Status WriteAll(int descriptor, absl::string_view content, absl::string_view filename) {
  std::size_t written = 0;
  while (written < content.size()) {
    const ssize_t count = write(descriptor, content.data() + written, content.size() - written);
    if (count < 0) {
      if (errno == EINTR) continue;
      return FileError("Cannot write file", filename, errno);
    }
    if (count == 0) return absl::UnavailableError(absl::StrCat("Short write to file: ", filename));
    written += static_cast<std::size_t>(count);
  }
  return absl::OkStatus();
}

absl::Status WriteFileAtomicallyAt(int parent_descriptor, absl::string_view filename, absl::string_view content) {
  if (content.size() > kMaxWriteBytes) {
    return absl::ResourceExhaustedError(absl::StrCat("Content exceeds the ", kMaxWriteBytes, " byte limit"));
  }
  const std::string name(filename);
  struct stat existing_info {};
  bool existing_file = false;
  if (fstatat(parent_descriptor, name.c_str(), &existing_info, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing_info.st_mode)) {
      return absl::PermissionDeniedError(absl::StrCat("Destination is not a regular file: ", filename));
    }
    existing_file = true;
  } else if (errno != ENOENT) {
    return FileError("Cannot inspect destination", filename, errno);
  }

  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string temporary_name;
  int temporary_descriptor = -1;
  for (int attempt = 0; attempt < 100; ++attempt) {
    temporary_name = absl::StrCat(".slop-write-", getpid(), "-", timestamp, "-", attempt);
    temporary_descriptor = openat(parent_descriptor, temporary_name.c_str(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (temporary_descriptor >= 0) break;
    if (errno != EEXIST) return FileError("Cannot create temporary file", filename, errno);
  }
  if (temporary_descriptor < 0) return absl::AlreadyExistsError("Cannot allocate a temporary file for atomic write");

  bool temporary_exists = true;
  absl::Cleanup remove_temporary = [&] {
    if (temporary_exists) unlinkat(parent_descriptor, temporary_name.c_str(), 0);
  };
  absl::Cleanup close_temporary = [&] {
    if (temporary_descriptor >= 0) close(temporary_descriptor);
  };

  if (existing_file && fchmod(temporary_descriptor, existing_info.st_mode & 0777) != 0) {
    return FileError("Cannot set file permissions", filename, errno);
  }
  RETURN_IF_ERROR(WriteAll(temporary_descriptor, content, filename));
  if (fsync(temporary_descriptor) != 0) return FileError("Cannot sync file", filename, errno);
  if (close(temporary_descriptor) != 0) {
    temporary_descriptor = -1;
    return FileError("Cannot close file", filename, errno);
  }
  temporary_descriptor = -1;

  if (renameat(parent_descriptor, temporary_name.c_str(), parent_descriptor, name.c_str()) != 0) {
    return FileError("Cannot replace file", filename, errno);
  }
  temporary_exists = false;
  return absl::OkStatus();
}

absl::Status ValidateFileLineOptions(const ReadFileOptions& options) {
  if ((options.start_line && *options.start_line < 1) || (options.end_line && *options.end_line < 1)) {
    return absl::InvalidArgumentError("Line numbers must be positive");
  }
  if (options.start_line && options.end_line && *options.start_line > *options.end_line) {
    return absl::InvalidArgumentError("start_line cannot be greater than end_line");
  }
  return absl::OkStatus();
}

std::vector<std::string> SplitLines(absl::string_view content) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < content.size()) {
    const std::size_t newline = content.find('\n', start);
    if (newline == absl::string_view::npos) {
      lines.emplace_back(content.substr(start));
      break;
    }
    lines.emplace_back(content.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

bool IsIgnored(absl::string_view name, const std::vector<std::string>& patterns) {
  const std::string entry_name(name);
  return std::any_of(patterns.begin(), patterns.end(), [&entry_name](const std::string& pattern) {
    return fnmatch(pattern.c_str(), entry_name.c_str(), FNM_PERIOD) == 0;
  });
}

enum class EntryKind {
  kFile,
  kDirectory,
  kSymlink,
  kOther,
};

using EntryVisitor = std::function<absl::Status(const std::string&, EntryKind)>;

absl::Status WalkDirectory(int directory_descriptor, const std::string& prefix, int current_depth, int max_depth,
                           const std::vector<std::string>& ignore_patterns, const EntryVisitor& visitor, bool* stop) {
  if (current_depth >= max_depth || *stop) return absl::OkStatus();
  const int duplicate_descriptor = dup(directory_descriptor);
  if (duplicate_descriptor < 0) return FileError("Cannot read directory", prefix, errno);
  DIR* directory = fdopendir(duplicate_descriptor);
  if (directory == nullptr) {
    const int error_number = errno;
    close(duplicate_descriptor);
    return FileError("Cannot read directory", prefix, error_number);
  }
  absl::Cleanup close_directory = [directory] { closedir(directory); };

  std::vector<std::string> names;
  errno = 0;
  while (dirent* entry = readdir(directory)) {
    if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' ||
                                    (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }
    names.emplace_back(entry->d_name);
  }
  if (errno != 0) return FileError("Cannot read directory", prefix, errno);
  std::sort(names.begin(), names.end());

  for (const std::string& name : names) {
    if (*stop) break;
    if (IsIgnored(name, ignore_patterns)) continue;
    struct stat info {};
    if (fstatat(directory_descriptor, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;
      return FileError("Cannot inspect directory entry", name, errno);
    }

    EntryKind kind = EntryKind::kOther;
    if (S_ISDIR(info.st_mode)) kind = EntryKind::kDirectory;
    if (S_ISREG(info.st_mode)) kind = EntryKind::kFile;
    if (S_ISLNK(info.st_mode)) kind = EntryKind::kSymlink;
    const std::string relative_path = prefix.empty() ? name : absl::StrCat(prefix, "/", name);
    RETURN_IF_ERROR(visitor(relative_path, kind));
    if (*stop || kind != EntryKind::kDirectory || current_depth + 1 >= max_depth) continue;

    const int child_descriptor = openat(directory_descriptor, name.c_str(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (child_descriptor < 0) {
      if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) continue;
      return FileError("Cannot open directory", relative_path, errno);
    }
    absl::Cleanup close_child = [child_descriptor] { close(child_descriptor); };
    RETURN_IF_ERROR(WalkDirectory(child_descriptor, relative_path, current_depth + 1, max_depth, ignore_patterns,
                                  visitor, stop));
    if (*stop) break;
  }
  return absl::OkStatus();
}

absl::Status AppendOutput(std::string* output, absl::string_view line) {
  const std::size_t extra = line.size() + (output->empty() ? 0 : 1);
  if (extra > kMaxOutputBytes - output->size()) {
    return absl::ResourceExhaustedError(absl::StrCat("Tool output exceeds the ", kMaxOutputBytes, " byte limit"));
  }
  if (!output->empty()) output->push_back('\n');
  output->append(line);
  return absl::OkStatus();
}

std::vector<std::string> IgnorePatterns(bool include_ignored, const std::vector<std::string>& explicit_patterns) {
  std::vector<std::string> patterns;
  if (!include_ignored) patterns = DefaultIgnorePatterns();
  patterns.insert(patterns.end(), explicit_patterns.begin(), explicit_patterns.end());
  return patterns;
}

bool MatchesPattern(absl::string_view line, absl::string_view pattern, const re2::RE2* regex, bool fixed_strings) {
  if (fixed_strings) return line.find(pattern) != absl::string_view::npos;
  const std::string text(line);
  return re2::RE2::PartialMatch(text, *regex);
}

std::vector<std::size_t> FindMatchIndices(const std::vector<std::string>& lines, absl::string_view pattern,
                                          const re2::RE2* regex, bool fixed_strings) {
  std::vector<std::size_t> matches;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (MatchesPattern(lines[index], pattern, regex, fixed_strings)) matches.push_back(index);
  }
  return matches;
}

absl::StatusOr<std::unique_ptr<re2::RE2>> CompileRegex(absl::string_view pattern) {
  re2::RE2::Options options;
  options.set_log_errors(false);
  options.set_max_mem(8 * 1024 * 1024);
  options.set_encoding(re2::RE2::Options::EncodingLatin1);
  auto regex = std::make_unique<re2::RE2>(std::string(pattern), options);
  if (!regex->ok()) {
    if (regex->error_code() == re2::RE2::ErrorPatternTooLarge) {
      return absl::ResourceExhaustedError("Regular expression exceeds the configured RE2 memory limit");
    }
    return absl::InvalidArgumentError(absl::StrCat("Invalid regular expression: ", regex->error()));
  }
  return regex;
}

absl::Status SearchFileContent(absl::string_view path, absl::string_view content, absl::string_view pattern,
                               const re2::RE2* regex, bool fixed_strings, int context, int limit,
                               std::size_t* searched_bytes, std::size_t* output_lines, std::string* output, bool* stop) {
  if (*stop) return absl::OkStatus();
  if (content.size() > kMaxGrepBytes - *searched_bytes) {
    return absl::ResourceExhaustedError(absl::StrCat("Search exceeds the ", kMaxGrepBytes, " byte limit"));
  }
  *searched_bytes += content.size();
  if (content.find('\0') != absl::string_view::npos) return absl::OkStatus();

  const std::vector<std::string> lines = SplitLines(content);
  std::vector<unsigned char> selected(lines.size(), 0);
  std::vector<unsigned char> matching(lines.size(), 0);
  for (const std::size_t index : FindMatchIndices(lines, pattern, regex, fixed_strings)) {
    matching[index] = 1;
    const std::size_t first = index > static_cast<std::size_t>(context) ? index - context : 0;
    const std::size_t last = std::min(lines.size(), index + static_cast<std::size_t>(context) + 1);
    for (std::size_t selected_index = first; selected_index < last; ++selected_index) selected[selected_index] = 1;
  }

  bool previous_selected = false;
  bool emitted_for_file = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (!selected[index]) {
      previous_selected = false;
      continue;
    }
    if (!previous_selected && emitted_for_file) RETURN_IF_ERROR(AppendOutput(output, "--"));
    const char* separator = matching[index] ? ":" : "-";
    RETURN_IF_ERROR(AppendOutput(output, absl::StrCat(path, separator, index + 1, separator, lines[index])));
    ++*output_lines;
    emitted_for_file = true;
    previous_selected = true;
    if (*output_lines >= static_cast<std::size_t>(limit)) {
      *stop = true;
      break;
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::vector<std::string>> ParseWorkspaceRelativePath(absl::string_view path, bool allow_root) {
  if (path.empty() || path.size() > 4096) {
    return absl::InvalidArgumentError("Path must be non-empty and no longer than 4096 bytes");
  }
  if (path.find('\0') != absl::string_view::npos) return absl::InvalidArgumentError("Path contains a NUL byte");
  if (path.front() == '/' || path.front() == '\\' || IsWindowsAbsolutePath(path)) {
    return absl::PermissionDeniedError("Absolute paths are not allowed");
  }

  std::vector<std::string> components;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t separator = path.find('/', start);
    const std::size_t end = separator == absl::string_view::npos ? path.size() : separator;
    const absl::string_view component = path.substr(start, end - start);
    if (!component.empty() && component != ".") {
      if (component == "..") return absl::PermissionDeniedError("Path traversal is not allowed");
      components.emplace_back(component);
    }
    if (separator == absl::string_view::npos) break;
    start = separator + 1;
  }

  if (components.empty() && !allow_root) return absl::InvalidArgumentError("Path must name a file");
  return components;
}

absl::StatusOr<std::vector<std::size_t>> FindMatchingLines(absl::string_view content, absl::string_view pattern,
                                                          bool fixed_strings) {
  if (pattern.empty()) return absl::InvalidArgumentError("pattern must be non-empty");
  if (pattern.find('\0') != absl::string_view::npos) return absl::InvalidArgumentError("pattern contains a NUL byte");
  if (pattern.size() > kMaxPatternBytes) {
    return absl::ResourceExhaustedError(absl::StrCat("pattern exceeds the ", kMaxPatternBytes, " byte limit"));
  }
  std::unique_ptr<re2::RE2> compiled_regex;
  if (!fixed_strings) {
    ASSIGN_OR_RETURN(compiled_regex, CompileRegex(pattern));
  }
  if (content.find('\0') != absl::string_view::npos) return std::vector<std::size_t>{};
  const std::vector<std::string> lines = SplitLines(content);
  const std::vector<std::size_t> indices =
      FindMatchIndices(lines, pattern, compiled_regex.get(), fixed_strings);
  std::vector<std::size_t> line_numbers;
  line_numbers.reserve(indices.size());
  for (const std::size_t index : indices) line_numbers.push_back(index + 1);
  return line_numbers;
}

absl::StatusOr<std::filesystem::path> ResolveWorkspaceDirectory(const std::filesystem::path& root,
                                                                absl::string_view path) {
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, true));
  ASSIGN_OR_RETURN(int directory_descriptor, OpenDirectory(root, components));
  absl::Cleanup close_directory = [directory_descriptor] { close(directory_descriptor); };

  std::filesystem::path resolved = root;
  for (const std::string& component : components) resolved /= component;
  return resolved;
}

absl::StatusOr<std::string> FormatFileLines(absl::string_view content, const ReadFileOptions& options) {
  RETURN_IF_ERROR(ValidateFileLineOptions(options));

  const std::vector<std::string> lines = SplitLines(content);
  if (lines.empty()) {
    if (options.start_line) return absl::InvalidArgumentError("start_line exceeds file length (0)");
    return std::string();
  }
  const std::size_t first = static_cast<std::size_t>(options.start_line.value_or(1) - 1);
  if (first >= lines.size()) {
    return absl::InvalidArgumentError(absl::StrCat("start_line exceeds file length (", lines.size(), ")"));
  }
  const std::size_t last = std::min(lines.size(), static_cast<std::size_t>(options.end_line.value_or(lines.size())));

  std::string output;
  for (std::size_t index = first; index < last; ++index) {
    if (options.line_numbers) output.append(absl::StrCat(index + 1, ": "));
    output.append(lines[index]);
    output.push_back('\n');
  }
  return output;
}

absl::StatusOr<std::string> ReadWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                              const ReadFileOptions& options) {
  RETURN_IF_ERROR(ValidateFileLineOptions(options));
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, false));
  ASSIGN_OR_RETURN(int parent_descriptor, OpenParentDirectory(root, components));
  absl::Cleanup close_parent = [parent_descriptor] { close(parent_descriptor); };
  ASSIGN_OR_RETURN(const std::string content, ReadFileAt(parent_descriptor, components.back(), kMaxReadBytes));
  return FormatFileLines(content, options);
}

absl::StatusOr<std::string> ListWorkspaceDirectory(const std::filesystem::path& root, absl::string_view path,
                                                   int depth, bool include_ignored,
                                                   const std::vector<std::string>& ignore_patterns) {
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, true));
  ASSIGN_OR_RETURN(int directory_descriptor, OpenDirectory(root, components));
  absl::Cleanup close_directory = [directory_descriptor] { close(directory_descriptor); };

  const int max_depth = std::max(1, std::min(8, depth));
  const std::vector<std::string> patterns = IgnorePatterns(include_ignored, ignore_patterns);
  std::string output;
  std::size_t entries = 0;
  bool stop = false;
  const EntryVisitor visitor = [&output, &entries](const std::string& relative, EntryKind kind) {
    if (++entries > kMaxDirectoryEntries) {
      return absl::ResourceExhaustedError(absl::StrCat("Directory contains more than ", kMaxDirectoryEntries,
                                                       " visible entries"));
    }
    if (kind == EntryKind::kDirectory) return AppendOutput(&output, absl::StrCat("Directory: ", relative, "/"));
    if (kind == EntryKind::kSymlink) return AppendOutput(&output, absl::StrCat("Symlink: ", relative));
    return AppendOutput(&output, absl::StrCat("File: ", relative));
  };
  RETURN_IF_ERROR(WalkDirectory(directory_descriptor, "", 0, max_depth, patterns, visitor, &stop));
  return output;
}

absl::StatusOr<std::string> GrepWorkspaceFiles(const std::filesystem::path& root, absl::string_view path,
                                               absl::string_view pattern, int context, int limit, bool fixed_strings,
                                               bool include_ignored,
                                               const std::vector<std::string>& ignore_patterns) {
  if (pattern.empty()) return absl::InvalidArgumentError("pattern must be non-empty");
  if (pattern.find('\0') != absl::string_view::npos) return absl::InvalidArgumentError("pattern contains a NUL byte");
  if (pattern.size() > kMaxPatternBytes) {
    return absl::ResourceExhaustedError(absl::StrCat("pattern exceeds the ", kMaxPatternBytes, " byte limit"));
  }
  if (context < 0 || context > 20) return absl::InvalidArgumentError("context must be between 0 and 20");
  if (limit < 1 || limit > 1000) return absl::InvalidArgumentError("limit must be between 1 and 1000");
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, true));

  std::unique_ptr<re2::RE2> compiled_regex;
  if (!fixed_strings) {
    ASSIGN_OR_RETURN(compiled_regex, CompileRegex(pattern));
  }
  const re2::RE2* regex = compiled_regex.get();

  std::string output;
  std::size_t searched_bytes = 0;
  std::size_t searched_files = 0;
  std::size_t output_lines = 0;
  bool stop = false;
  const std::vector<std::string> patterns = IgnorePatterns(include_ignored, ignore_patterns);
  auto search_file = [&](absl::string_view relative_path) -> absl::Status {
    if (++searched_files > kMaxGrepFiles) {
      return absl::ResourceExhaustedError(absl::StrCat("Search includes more than ", kMaxGrepFiles, " files"));
    }
    ASSIGN_OR_RETURN(const std::string content, ReadWorkspaceFile(root, relative_path, ReadFileOptions{}));
    return SearchFileContent(relative_path, content, pattern, regex, fixed_strings, context, limit, &searched_bytes,
                             &output_lines, &output, &stop);
  };

  std::string base_path = absl::StrJoin(components, "/");
  if (components.empty()) {
    ASSIGN_OR_RETURN(int directory_descriptor, OpenDirectory(root, components));
    absl::Cleanup close_directory = [directory_descriptor] { close(directory_descriptor); };
    const EntryVisitor visitor = [&](const std::string& relative, EntryKind kind) {
      if (kind != EntryKind::kFile) return absl::OkStatus();
      return search_file(relative);
    };
    RETURN_IF_ERROR(WalkDirectory(directory_descriptor, "", 0, kMaxGrepDepth, patterns, visitor, &stop));
    return output;
  }

  ASSIGN_OR_RETURN(int parent_descriptor, OpenParentDirectory(root, components));
  absl::Cleanup close_parent = [parent_descriptor] { close(parent_descriptor); };
  struct stat info {};
  const std::string basename = components.back();
  if (fstatat(parent_descriptor, basename.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
    return FileError("Cannot inspect search path", path, errno);
  }
  if (S_ISREG(info.st_mode)) {
    ++searched_files;
    ASSIGN_OR_RETURN(const std::string content, ReadFileAt(parent_descriptor, basename, kMaxReadBytes));
    RETURN_IF_ERROR(SearchFileContent(path, content, pattern, regex, fixed_strings, context, limit, &searched_bytes,
                                      &output_lines, &output, &stop));
    return output;
  }
  if (!S_ISDIR(info.st_mode)) return absl::PermissionDeniedError("Search path must be a regular file or directory");

  const int directory_descriptor = openat(parent_descriptor, basename.c_str(),
                                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_descriptor < 0) return FileError("Cannot open search directory", path, errno);
  absl::Cleanup close_directory = [directory_descriptor] { close(directory_descriptor); };
  const EntryVisitor visitor = [&](const std::string& relative, EntryKind kind) {
    if (kind != EntryKind::kFile) return absl::OkStatus();
    return search_file(relative);
  };
  RETURN_IF_ERROR(WalkDirectory(directory_descriptor, base_path, 0, kMaxGrepDepth, patterns, visitor, &stop));
  return output;
}

absl::StatusOr<std::string> WriteWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                               absl::string_view content) {
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, false));
  ASSIGN_OR_RETURN(int parent_descriptor, OpenParentDirectory(root, components));
  absl::Cleanup close_parent = [parent_descriptor] { close(parent_descriptor); };
  RETURN_IF_ERROR(WriteFileAtomicallyAt(parent_descriptor, components.back(), content));
  return absl::StrCat("File written successfully:\nPath: ", path, "\nBytes written: ", content.size(), "\n");
}

absl::StatusOr<slop::TextEditResult> EditWorkspaceFile(const std::filesystem::path& root, absl::string_view path,
                                                       const nlohmann::json& edits) {
  RETURN_IF_ERROR(ValidateTextEdits(edits));
  ASSIGN_OR_RETURN(const std::vector<std::string> components, ParseWorkspaceRelativePath(path, false));
  ASSIGN_OR_RETURN(int parent_descriptor, OpenParentDirectory(root, components));
  absl::Cleanup close_parent = [parent_descriptor] { close(parent_descriptor); };
  ASSIGN_OR_RETURN(const std::string original, ReadFileAt(parent_descriptor, components.back(), kMaxReadBytes));
  ASSIGN_OR_RETURN(slop::TextEditResult result, slop::ApplyTextEdits(original, edits));
  if (result.content.size() > kMaxWriteBytes) {
    return absl::ResourceExhaustedError(absl::StrCat("Edited file exceeds the ", kMaxWriteBytes, " byte limit"));
  }
  RETURN_IF_ERROR(WriteFileAtomicallyAt(parent_descriptor, components.back(), result.content));
  return result;
}

}  // namespace slop::mcp::local_tools
