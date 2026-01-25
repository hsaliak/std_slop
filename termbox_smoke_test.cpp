#include "termbox2.h"

#include <iostream>

int main() {
  int ret = tb_init();
  if (ret != 0) {
    std::cerr << "tb_init() failed with error code " << ret << "\n";
    // In some CI environments, there might not be a TTY.
    // We still consider the integration successful if it compiles and links.
    if (ret == TB_ERR_INIT_OPEN) {
      std::cout << "Detected no TTY (expected in some environments). "
                << "Integration looks good!\n";
      return 0;
    }
    return 1;
  }

  std::cout << "termbox2 initialized successfully. Terminal size: " << tb_width()
            << "x" << tb_height() << "\n";

  tb_shutdown();
  std::cout << "termbox2 shut down successfully.\n";
  return 0;
}
