#include "core/shell_util.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"

#include <sys/wait.h>

namespace slop {

absl::StatusOr<CommandResult> RunCommand(std::string_view command,
                                         std::shared_ptr<CancellationRequest> cancellation) {
  LOG(INFO) << "Running command: " << command;
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
    // Set process group ID to own PID so we can kill the whole group
    setpgid(0, 0);

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    execl("/bin/sh", "sh", "-c", std::string(command).c_str(), nullptr);
    _exit(1);
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

  auto cleanup_child = [&](int sig) {
    LOG(INFO) << "Cleaning up child process " << pid << " with signal " << sig;
    kill(-pid, sig);  // Kill process group

    // Give it a moment to shut down
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int status;
    if (waitpid(pid, &status, WNOHANG) == 0) {
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  };

  while (stdout_open || stderr_open) {
    if (cancellation && cancellation->IsCancelled()) {
      LOG(INFO) << "Command cancelled via CancellationRequest";
      cleanup_child(SIGTERM);
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      return absl::CancelledError("Command cancelled");
    }

    int ret = poll(fds.data(), fds.size(), 50);  // Shorter timeout for faster cancellation check
    if (ret == -1) {
      if (errno == EINTR) continue;
      break;
    }

    if (ret == 0) continue;

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

  LOG(INFO) << "Command exited with code " << exit_code;

  return CommandResult{stdout_str, stderr_str, exit_code};
}

std::string EscapeShellArg(std::string_view arg) {
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

bool IsEscPressed() {
  static auto last_check = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count() < 100) {
    return false;
  }
  last_check = now;

  if (!isatty(STDIN_FILENO)) {
    return false;
  }

  struct termios oldt, newt;
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) return false;
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) return false;
  int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (oldf == -1) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return false;
  }
  if (fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK) == -1) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return false;
  }

  int ch = getchar();

  (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  (void)fcntl(STDIN_FILENO, F_SETFL, oldf);

  return ch == 27;
}

}  // namespace slop
