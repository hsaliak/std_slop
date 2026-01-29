#include "core/shell_util.h"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <vector>

#include "absl/status/status.h"

#include <sys/wait.h>

namespace slop {

absl::StatusOr<CommandResult> RunCommand(const std::string& command) {
  std::array<int, 2> stdout_pipe;
  std::array<int, 2> stderr_pipe;

  if (pipe(stdout_pipe.data()) == -1) {
    return absl::InternalError("Failed to create stdout pipe");
  }
  if (pipe(stderr_pipe.data()) == -1) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return absl::InternalError("Failed to create stderr pipe");
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    return absl::InternalError("Failed to fork");
  }

  if (pid == 0) {
    // Child process
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }

  // Parent process
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  std::string stdout_str;
  std::string stderr_str;
  std::array<char, 4096> buffer;

  std::vector<pollfd> fds(2);
  fds[0].fd = stdout_pipe[0];
  fds[0].events = POLLIN;
  fds[1].fd = stderr_pipe[0];
  fds[1].events = POLLIN;

  bool stdout_open = true;
  bool stderr_open = true;

  while (stdout_open || stderr_open) {
    int ret = poll(fds.data(), fds.size(), -1);
    if (ret == -1) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < 2; ++i) {
      if (fds[i].revents & (POLLIN | POLLHUP)) {
        ssize_t bytes = read(fds[i].fd, buffer.data(), buffer.size());
        if (bytes > 0) {
          if (i == 0) {
            stdout_str.append(buffer.data(), bytes);
          } else {
            stderr_str.append(buffer.data(), bytes);
          }
        } else if (bytes == 0 || (bytes == -1 && errno != EINTR && errno != EAGAIN)) {
          if (i == 0)
            stdout_open = false;
          else
            stderr_open = false;
          fds[i].fd = -1;  // Stop polling this fd
        }
      } else if (fds[i].revents & (POLLERR | POLLNVAL)) {
        if (i == 0)
          stdout_open = false;
        else
          stderr_open = false;
        fds[i].fd = -1;
      }
    }
  }

  close(stdout_pipe[0]);
  close(stderr_pipe[0]);

  int status;
  waitpid(pid, &status, 0);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  return CommandResult{stdout_str, stderr_str, exit_code};
}

std::string EscapeShellArg(const std::string& arg) {
  std::string escaped = "'";
  for (char c : arg) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped += c;
    }
  }
  escaped += "'";
  return escaped;
}

}  // namespace slop
