#include "interface/animator.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

namespace slop {
namespace {

size_t GetTerminalWidthOrDefault() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
    return static_cast<size_t>(w.ws_col);
  }
  return 80;
}

}  // namespace

AsyncAnimator::AsyncAnimator() : is_running_(false) {}

AsyncAnimator::~AsyncAnimator() { Stop(); }

void AsyncAnimator::Start() {
  if (is_running_) return;
  is_running_ = true;
  start_time_ = std::chrono::steady_clock::now();
  std::cout << "\033[?25l";
  // Reserve space for 1 line
  std::cout << "\n\033[1A";
  thread_ = std::thread(&AsyncAnimator::RenderLoop, this);
}

void AsyncAnimator::Stop() {
  if (!is_running_) return;
  is_running_ = false;
  if (thread_.joinable()) thread_.join();
  // Clear the spinner line
  std::cout << "\r\033[2K";
  std::cout << "\033[?25h" << std::flush;
}

void AsyncAnimator::RenderLoop() {
  constexpr const char* kTimerColor = "\x1b[38;2;146;131;116m";   // Gruvbox gray
  constexpr const char* kWaveColors[] = {
      "\x1b[38;2;215;153;33m",   // Gruvbox yellow
      "\x1b[38;2;142;192;124m",  // Gruvbox aqua
      "\x1b[38;2;69;133;136m",   // Gruvbox blue
      "\x1b[38;2;177;98;134m",   // Gruvbox purple
  };
  constexpr const char* kReset = "\033[0m";
  const std::vector<std::string> wave = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█", "▇", "▆", "▅", "▄", "▃", "▂"};
  size_t phase = 0;
  constexpr int kTickMs = 90;

  while (is_running_) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
    double seconds = elapsed_ms / 1000.0;

    char time_buf[32];
    std::snprintf(time_buf, sizeof(time_buf), "[%.1fs] ", seconds);
    const std::string prefix(time_buf);

    const size_t terminal_width = GetTerminalWidthOrDefault();
    const size_t prefix_width = prefix.size();
    const size_t wave_width = terminal_width > prefix_width ? terminal_width - prefix_width : 1;

    std::string frame = "\r\033[2K";
    frame += kTimerColor;
    frame += prefix;
    frame += kReset;

    for (size_t i = 0; i < wave_width; ++i) {
      const size_t wave_idx = (phase + i) % wave.size();
      const size_t color_idx = (phase + i) % (sizeof(kWaveColors) / sizeof(kWaveColors[0]));
      frame += kWaveColors[color_idx];
      frame += wave[wave_idx];
    }
    frame += kReset;

    std::cout << frame << std::flush;

    phase = (phase + 1) % wave.size();

    for (int i = 0; i < kTickMs / 10 && is_running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

}  // namespace slop
