#include "core/shell_util.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include <sys/wait.h>

namespace slop {

namespace {
struct GlobalTerminalState {
  int refcount = 0;
  struct termios oldt;
  int oldf = -1;
  bool active = false;
};
GlobalTerminalState g_terminal_state;
}  // namespace

ScopedRawMode::ScopedRawMode() {
  if (g_terminal_state.refcount++ == 0) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_terminal_state.oldt) == 0) {
      struct termios newt = g_terminal_state.oldt;
      newt.c_lflag &= ~(ICANON | ECHO);
      if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0) {
        g_terminal_state.oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (g_terminal_state.oldf != -1) {
          fcntl(STDIN_FILENO, F_SETFL, g_terminal_state.oldf | O_NONBLOCK);
        }
        g_terminal_state.active = true;
      }
    }
  }
}

ScopedRawMode::~ScopedRawMode() {
  if (--g_terminal_state.refcount == 0) {
    if (g_terminal_state.active) {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_terminal_state.oldt);
      if (g_terminal_state.oldf != -1) {
        fcntl(STDIN_FILENO, F_SETFL, g_terminal_state.oldf);
      }
      g_terminal_state.active = false;
    }
  }
}

bool ScopedRawMode::IsActive() const { return g_terminal_state.active; }

absl::StatusOr<CommandResult> RunCommand(std::string_view command, std::shared_ptr<CancellationRequest> cancellation,
                                         std::string_view input, int timeout_seconds) {
  LOG(INFO) << "Running command: " << command;

  int stdout_pipe[2];
  int stderr_pipe[2];
  int stdin_pipe[2];

  if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1 || pipe(stdin_pipe) == -1) {
    return absl::InternalError("Failed to create pipes");
  }

  pid_t pid = fork();
  if (pid == -1) {
    return absl::InternalError("Failed to fork");
  }

  if (pid == 0) {
    setsid();
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    dup2(stdin_pipe[0], STDIN_FILENO);

    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);

    execl("/bin/sh", "sh", "-c", std::string(command).c_str(), nullptr);
    exit(1);
  }

  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  close(stdin_pipe[0]);

  auto set_nonblocking = [](int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  };
  set_nonblocking(stdout_pipe[0]);
  set_nonblocking(stderr_pipe[0]);
  set_nonblocking(stdin_pipe[1]);

  std::string stdout_str;
  std::string stderr_str;
  std::vector<char> buffer(4096);

  size_t stdin_written = 0;
  bool stdin_open = !input.empty();
  bool stdout_open = true;
  bool stderr_open = true;

  if (!stdin_open) close(stdin_pipe[1]);

  auto start_time = std::chrono::steady_clock::now();
  std::vector<struct pollfd> fds = {
      {stdout_pipe[0], POLLIN, 0},
      {stderr_pipe[0], POLLIN, 0},
      {stdin_open ? stdin_pipe[1] : -1, POLLOUT, 0},
  };

  auto cleanup_child = [&](int sig) {
    kill(-pid, sig);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int status;
    if (waitpid(pid, &status, WNOHANG) == 0) {
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  };

  while (stdout_open || stderr_open || stdin_open) {
    if (cancellation && cancellation->IsCancelled()) {
      cleanup_child(SIGTERM);
      if (stdout_open) close(stdout_pipe[0]);
      if (stderr_open) close(stderr_pipe[0]);
      if (stdin_open) close(stdin_pipe[1]);
      return absl::CancelledError("Command cancelled");
    }

    if (timeout_seconds > 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= timeout_seconds) {
        cleanup_child(SIGKILL);
        if (stdout_open) close(stdout_pipe[0]);
        if (stderr_open) close(stderr_pipe[0]);
        if (stdin_open) close(stdin_pipe[1]);
        return absl::DeadlineExceededError("Command timed out");
      }
    }

    int ret = poll(fds.data(), fds.size(), 50);
    if (ret <= 0) {
        if (ret == -1 && errno == EINTR) continue;
        if (ret == 0) continue;
        break;
    }

    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd != -1 && (fds[i].revents & (POLLIN | POLLHUP))) {
        ssize_t bytes = read(fds[i].fd, buffer.data(), buffer.size());
        if (bytes > 0) {
          if (i == 0) stdout_str.append(buffer.data(), bytes);
          else stderr_str.append(buffer.data(), bytes);
        } else if (bytes == 0 || (bytes == -1 && errno != EAGAIN)) {
          if (i == 0) stdout_open = false; else stderr_open = false;
          close(fds[i].fd); fds[i].fd = -1;
        }
      }
    }

    if (stdin_open && (fds[2].revents & POLLOUT)) {
      ssize_t bytes = write(stdin_pipe[1], input.data() + stdin_written, input.size() - stdin_written);
      if (bytes > 0) {
        stdin_written += bytes;
        if (stdin_written == input.size()) {
          stdin_open = false;
          close(stdin_pipe[1]);
          fds[2].fd = -1;
        }
      } else if (bytes == -1 && errno != EAGAIN) {
        stdin_open = false;
        close(stdin_pipe[1]);
        fds[2].fd = -1;
      }
    }
  }

  if (stdout_open) close(stdout_pipe[0]);
  if (stderr_open) close(stderr_pipe[0]);
  if (stdin_open) close(stdin_pipe[1]);

  int status;
  waitpid(pid, &status, 0);
  return CommandResult{stdout_str, stderr_str, WIFEXITED(status) ? WEXITSTATUS(status) : -1};
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

  if (g_terminal_state.active) {
    int ch = getchar();
    return ch == 27;
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

std::string GetHomeDir() {
  const char* home = std::getenv("HOME");
  if (home) {
    return {home};
  }
  struct passwd* pw = getpwuid(getuid());
  if (pw) {
    return {pw->pw_dir};
  }
  return "";
}

}  // namespace slop
