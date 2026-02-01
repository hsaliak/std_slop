#include "interface/ui.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <sstream>
#include <iostream>

namespace slop {
namespace {

TEST(UIThreadSafetyTest, ConcurrentPrinting) {
  // This test doesn't check for visual correctness (hard to do with stdout),
  // but running it under TSAN will detect if the ui_mutex is missing or broken.
  
  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([i]() {
      for (int j = 0; j < 50; ++j) {
        PrintToolCallMessage("tool_" + std::to_string(i), "{\"arg\":" + std::to_string(j) + "}", "  ", 0);
        PrintToolResultMessage("tool_" + std::to_string(i), "result_" + std::to_string(j), "completed", "  ");
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }
}

}  // namespace
}  // namespace slop
