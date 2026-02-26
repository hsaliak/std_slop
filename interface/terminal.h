#ifndef SLOP_INTERFACE_TERMINAL_H_
#define SLOP_INTERFACE_TERMINAL_H_

#include "interface/color.h"

#include <string>

namespace slop {

size_t GetTerminalWidth();
std::string WrapText(const std::string& text, size_t width, const std::string& prefix = "", const std::string& first_line_prefix = "");
void SetupTerminal();
std::string ReadLine(const std::string& modeline);

}  // namespace slop

#endif  // SLOP_INTERFACE_TERMINAL_H_
