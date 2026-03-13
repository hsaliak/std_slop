#include "interface/animator.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace slop {

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
  const char* spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  constexpr int kSpinnerFrames = 10;
  constexpr int kTrailWidth = 5;
  int spinner_idx = 0;
  // Gruvbox colors
  const struct {
    int r, g, b;
  } colors[] = {
      {204, 36, 29},    // Red
      {152, 151, 26},   // Green
      {215, 153, 33},   // Yellow
      {69, 133, 136},   // Blue
      {177, 98, 134},   // Purple
      {104, 157, 106},  // Aqua
      {214, 93, 14}     // Orange
  };
  int color_idx = 0;
  int num_colors = sizeof(colors) / sizeof(colors[0]);

  while (is_running_) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
    double seconds = elapsed_ms / 1000.0;

    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), " (%.1fs)", seconds);

    std::string frame = "\r\033[2K\033[38;2;";
    frame += std::to_string(colors[color_idx].r) + ";" + std::to_string(colors[color_idx].g) + ";" +
             std::to_string(colors[color_idx].b) + "m";
    frame += spinner[spinner_idx];
    frame += " ";
    for (int i = 0; i < kTrailWidth; ++i) {
      frame += spinner[(spinner_idx + i + 1) % kSpinnerFrames];
    }
    frame += "\033[0m";
    frame += " \033[38;2;142;192;124m";
    frame += time_buf;
    frame += "\033[0m";

    std::cout << frame << std::flush;

    spinner_idx = (spinner_idx + 1) % kSpinnerFrames;
    color_idx = (color_idx + 1) % num_colors;

    for (int i = 0; i < 5 && is_running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

}  // namespace slop
