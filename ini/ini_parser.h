#ifndef SLOP_INI_INI_PARSER_H_
#define SLOP_INI_INI_PARSER_H_

#include <map>
#include <string>
#include <string_view>

namespace slop {

// Represents a simple INI configuration.
// Sections are maps of key-value pairs.
using IniSection = std::map<std::string, std::string>;
using IniConfig = std::map<std::string, IniSection>;

// Parses an INI string and returns an IniConfig object. Environment expansion
// can be disabled for fields that must preserve literal command arguments.
// Basic format:
// [section]
// key = value
// # comment
IniConfig ParseIni(std::string_view content, bool expand_env_vars = true);

}  // namespace slop

#endif  // SLOP_INI_INI_PARSER_H_
