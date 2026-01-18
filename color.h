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

struct Style {
    const char* fg = "";
    const char* bg = "";
    bool bold = false;

    static Style Default() { return {ansi::White, "", false}; }
    static Style User() { return {ansi::Green, "", true}; }
    static Style Assistant() { return {ansi::Cyan, "", false}; }
    static Style System() { return {ansi::Yellow, "", false}; }
    static Style Error() { return {ansi::Magenta, "", true}; }
    static Style Tool() { return {ansi::Grey, "", false}; }
    static Style Subtle() { return {ansi::Grey, "", false}; }
};

inline std::string Colorize(const std::string& text, const char* bg_background,
                             const char* fg_foreground = ansi::White) {
    return std::string(bg_background) + std::string(fg_foreground) + text +
           ansi::Reset;
}

inline std::string ApplyStyle(const std::string& text, const Style& style) {
    std::string res;
    if (style.bold) res += ansi::Bold;
    if (style.bg && *style.bg) res += style.bg;
    if (style.fg && *style.fg) res += style.fg;
    res += text;
    res += ansi::Reset;
    return res;
}

} // namespace slop

#endif  // COLOR_H_
