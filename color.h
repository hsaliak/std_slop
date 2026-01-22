#ifndef COLOR_H_
#define COLOR_H_

#include <string>

namespace ansi {
    // Reset code
    constexpr const char* Reset = "\033[0m";

    // Text style
    constexpr const char* Bold = "\033[1m";

    // Background colors
    constexpr const char* BlueBg = "\033[44m";      // Blue background
    constexpr const char* CyanBg = "\033[46m";      // Cyan background
    constexpr const char* GreyBg = "\033[100m";     // Grey (bright black) background

    // Foreground (text) color
    constexpr const char* White = "\033[37m";
    constexpr const char* Black = "\033[30m";
    constexpr const char* Blue = "\033[34m";
    constexpr const char* Cyan = "\033[36m";
    constexpr const char* Grey = "\033[90m";
    constexpr const char* Green = "\033[32m";
    constexpr const char* Yellow = "\033[33m";
    constexpr const char* Magenta = "\033[35m";
}  // namespace ansi

namespace slop {

inline std::string Colorize(const std::string& text, const char* bg_background,
                             const char* fg_foreground = ansi::White) {
    return std::string(bg_background) + std::string(fg_foreground) + text +
           ansi::Reset;
}

} // namespace slop

#endif  // COLOR_H_
