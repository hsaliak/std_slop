#ifndef SLOP_TERMINAL_H_
#define SLOP_TERMINAL_H_

#include <string>

namespace slop {

size_t GetTerminalWidth();
std::string WrapText(const std::string& text, size_t width, const std::string& prefix = "", const std::string& first_line_prefix = "");
void SetupTerminal();

}  // namespace slop

#endif  // SLOP_TERMINAL_H_
