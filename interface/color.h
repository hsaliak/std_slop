#ifndef SLOP_INTERFACE_COLOR_H_
#define SLOP_INTERFACE_COLOR_H_

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
constexpr const char* Camera = "📷";
constexpr const char* MagnifyingGlass = "🔍";
constexpr const char* Sparkles = "✨";
constexpr const char* Hammer = "🔨";
constexpr const char* Gear = "⚙️";
constexpr const char* Link = "🔗";
constexpr const char* CallArrow = "❯";
constexpr const char* ResultConnector = "┗━";
}  // namespace icons

namespace ansi {
// Gruvbox Dark Palette
namespace gruvbox {
constexpr const char* Aqua = "\x1b[38;2;142;192;124m";
constexpr const char* Blue = "\x1b[38;2;69;133;136m";
constexpr const char* Yellow = "\x1b[38;2;215;153;33m";
constexpr const char* Red = "\x1b[38;2;204;36;29m";
constexpr const char* Gray = "\x1b[38;2;146;131;116m";
constexpr const char* DarkGray = "\x1b[38;2;60;56;54m";
constexpr const char* Bg = "\x1b[48;2;60;56;54m";
constexpr const char* Purple = "\x1b[38;2;177;98;134m";
}  // namespace gruvbox

constexpr const char* Reset = "\033[0m";
constexpr const char* Bold = "\033[1m";
constexpr const char* Dim = "\033[2m";
constexpr const char* Underline = "\033[4m";
constexpr const char* Blink = "\033[5m";
constexpr const char* Reverse = "\033[7m";
constexpr const char* Hidden = "\033[8m";

// Standard Foreground (text) color - Mapped to Gruvbox
constexpr const char* White = "\x1b[38;2;235;219;178m";  // Gruvbox light0
constexpr const char* Black = gruvbox::DarkGray;
constexpr const char* Blue = gruvbox::Blue;
constexpr const char* Cyan = gruvbox::Aqua;
constexpr const char* Grey = gruvbox::Gray;
constexpr const char* LightGrey = "\x1b[38;2;168;153;132m";  // Gruvbox gray_245
constexpr const char* MildGrey = gruvbox::Gray;
constexpr const char* Green = gruvbox::Aqua;  // Using Aqua for Green as requested
constexpr const char* Yellow = gruvbox::Yellow;
constexpr const char* Magenta = gruvbox::Purple;
constexpr const char* Red = gruvbox::Red;

constexpr const char* Thought = Grey;
constexpr const char* Assistant = White;
constexpr const char* Metadata = Grey;
constexpr const char* UserLabel = Green;
constexpr const char* EchoBg = gruvbox::Bg;
constexpr const char* EchoFg = White;
constexpr const char* Warning = Yellow;
constexpr const char* Logo = Cyan;
constexpr const char* SystemLabel = Yellow;

// Mode labels
constexpr const char* StandardMode = Cyan;
constexpr const char* MailMode = Blue;

namespace theme {
namespace markdown {
inline constexpr const char* Header = "\033[1m\x1b[38;2;142;192;124m";  // Bold Aqua
inline constexpr const char* HeaderMarker = gruvbox::Gray;
inline constexpr const char* Bold = "\033[1m";
inline constexpr const char* Italic = "\033[3m";
inline constexpr const char* CodeInline = "\x1b[38;2;142;192;124m";  // Aqua
inline constexpr const char* CodeBlock = LightGrey;
inline constexpr const char* LinkText = "\x1b[38;2;69;133;136m\033[4m";  // Blue Underline
inline constexpr const char* LinkUrl = gruvbox::Gray;
inline constexpr const char* ListMarker = gruvbox::Yellow;
inline constexpr const char* Quote = gruvbox::Purple;
inline constexpr const char* HorizontalRule = gruvbox::Gray;
inline constexpr const char* TableBorder = gruvbox::Gray;
inline constexpr const char* TableHeader = "\033[1m\x1b[38;2;142;192;124m";  // Bold Aqua
}  // namespace markdown

namespace syntax {
inline constexpr const char* Keyword = gruvbox::Red;
inline constexpr const char* Function = gruvbox::Blue;
inline constexpr const char* Type = gruvbox::Yellow;
inline constexpr const char* String = gruvbox::Aqua;
inline constexpr const char* Comment = gruvbox::Gray;
inline constexpr const char* Number = gruvbox::Purple;
inline constexpr const char* Operator = gruvbox::Aqua;
inline constexpr const char* Preproc = gruvbox::Aqua;
inline constexpr const char* Constant = gruvbox::Red;
inline constexpr const char* Variable = White;
inline constexpr const char* Label = gruvbox::Yellow;
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
    if (s[i] == '\033' && i + 1 < s.length() && s[i + 1] == '[') {
      i += 2;
      while (i < s.length() && (s[i] < 0x40 || s[i] > 0x7E)) {
        i++;
      }
    } else {
      if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
        len++;
      }
    }
  }
  return len;
}

}  // namespace slop

#endif  // SLOP_INTERFACE_COLOR_H_
