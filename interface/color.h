#ifndef COLOR_H_
#define COLOR_H_

#include <string>

namespace icons {
constexpr const char* Success = "✅";
constexpr const char* Error = "❌";
constexpr const char* Warning = "⚠️";
constexpr const char* Info = "ℹ️";
constexpr const char* Tool = "🛠️";
constexpr const char* Thought = "🧠";
constexpr const char* Output = "📤";
constexpr const char* Input = "📥";
constexpr const char* Memo = "📝";
constexpr const char* Skill = "🎓";
constexpr const char* Session = "🕒";
constexpr const char* Robot = "🤖";
constexpr const char* Mailbox = "📬";
constexpr const char* CallArrow = "❯";
constexpr const char* ResultConnector = "┗━";
}  // namespace icons

namespace ansi {
// Reset code
constexpr const char* Reset = "\033[0m";

// Text style
constexpr const char* Bold = "\033[1m";

// Background colors
constexpr const char* BlueBg = "\033[44m";   // Blue background
constexpr const char* CyanBg = "\033[46m";   // Cyan background
constexpr const char* GreyBg = "\033[100m";  // Grey (bright black) background

// Foreground (text) color
constexpr const char* White = "\033[37m";
constexpr const char* Black = "\033[30m";
constexpr const char* Blue = "\033[34m";
constexpr const char* Cyan = "\033[36m";
constexpr const char* Grey = "\033[90m";
constexpr const char* LightGrey = "\033[38;5;251m";
constexpr const char* MildGrey = "\033[38;5;244m";
constexpr const char* Green = "\033[32m";
constexpr const char* Yellow = "\033[33m";
constexpr const char* Magenta = "\033[35m";
constexpr const char* Red = "\033[31m";
constexpr const char* Thought = Grey;
constexpr const char* Assistant = White;
constexpr const char* Metadata = Grey;
constexpr const char* UserLabel = Green;
constexpr const char* EchoBg = GreyBg;
constexpr const char* EchoFg = White;
constexpr const char* Warning = Yellow;
constexpr const char* Logo = Cyan;
constexpr const char* SystemLabel = Yellow;

namespace theme {
namespace markdown {
inline constexpr const char* Header = "\033[1;36m";      // Bold Cyan
inline constexpr const char* HeaderMarker = "\033[90m";  // Grey
inline constexpr const char* Bold = "\033[1m";
inline constexpr const char* Italic = "\033[3m";
inline constexpr const char* CodeInline = "\e[38;5;81m";  // Green
inline constexpr const char* CodeBlock = LightGrey;
inline constexpr const char* LinkText = "\033[34;4m";      // Blue Underline
inline constexpr const char* LinkUrl = "\033[90m";         // Grey
inline constexpr const char* ListMarker = "\033[33m";      // Yellow
inline constexpr const char* Quote = "\033[35m";           // Magenta
inline constexpr const char* HorizontalRule = "\033[90m";  // Grey
inline constexpr const char* TableBorder = "\033[90m";     // Grey
inline constexpr const char* TableHeader = "\033[1;36m";   // Bold Cyan
}  // namespace markdown

namespace syntax {
inline constexpr const char* Keyword = "\033[35m";   // Magenta
inline constexpr const char* Function = "\033[34m";  // Blue
inline constexpr const char* Type = "\033[33m";      // Yellow
inline constexpr const char* String = "\033[32m";    // Green
inline constexpr const char* Comment = "\033[90m";   // Grey
inline constexpr const char* Number = "\033[31m";    // Red
inline constexpr const char* Operator = "\033[36m";  // Cyan
inline constexpr const char* Preproc = "\033[36m";   // Cyan
inline constexpr const char* Constant = "\033[31m";  // Red
inline constexpr const char* Variable = "\033[37m";  // White
inline constexpr const char* Label = "\033[33m";     // Yellow
}  // namespace syntax
}  // namespace theme
}  // namespace ansi

namespace slop {

inline std::string Colorize(const std::string& text, const char* bg_background,
                            const char* fg_foreground = ansi::White) {
  return std::string(bg_background) + std::string(fg_foreground) + text + ansi::Reset;
}

/**
 * @brief Calculates the printable length of a string, excluding ANSI escape codes.
 *
 * Handles multi-byte UTF-8 characters and standard ANSI SGR (Select Graphic Rendition)
 * sequences to determine how many columns the string will occupy in the terminal.
 *
 * @param s The string to measure.
 * @return size_t The number of visible terminal columns.
 */
inline size_t VisibleLength(std::string_view s) {
  size_t len = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    // Detect start of ANSI escape sequence
    if (s[i] == '\033' && i + 1 < s.length() && s[i + 1] == '[') {
      i += 2;
      // Skip characters until the termination character of the sequence (0x40-0x7E)
      while (i < s.length() && (s[i] < 0x40 || s[i] > 0x7E)) {
        i++;
      }
    } else {
      // For UTF-8, only count the start byte of a character sequence (bytes not 10xxxxxx)
      if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
        len++;
      }
    }
  }
  return len;
}

}  // namespace slop

#endif  // COLOR_H_
