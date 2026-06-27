#!/bin/sh
# Generates a C++ header containing built-in skill prompt data.
set -eu

usage() {
  echo "usage: $0 --out <output-header> <skill.md>..." >&2
}

if [ "$#" -lt 3 ] || [ "$1" != "--out" ]; then
  usage
  exit 1
fi

out=$2
shift 2

cpp_string_literal() {
  # Escape backslashes and double quotes for a C++ string literal.
  sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$/"/'
}

emit_skill() {
  path=$1
  name_line=$(sed -n '1p' "$path")
  description_line=$(sed -n '2p' "$path")
  blank_line=$(sed -n '3p' "$path")

  case "$name_line" in
    "# Name: "*) name=${name_line#"# Name: "} ;;
    *) echo "$path: first line must be '# Name: <skill-name>'" >&2; exit 1 ;;
  esac
  case "$description_line" in
    "# Description: "*) description=${description_line#"# Description: "} ;;
    *) echo "$path: second line must be '# Description: <description>'" >&2; exit 1 ;;
  esac
  if [ -n "$blank_line" ]; then
    echo "$path: third line must be blank" >&2
    exit 1
  fi
  if [ -z "$name" ]; then
    echo "$path: skill name must not be empty" >&2
    exit 1
  fi
  if [ -z "$description" ]; then
    echo "$path: description must not be empty" >&2
    exit 1
  fi
  if ! sed '1,3d' "$path" | grep -q '[^[:space:]]'; then
    echo "$path: prompt body must not be empty" >&2
    exit 1
  fi
  if sed '1,3d' "$path" | grep -q ')SKILL"'; then
    echo "$path: prompt body contains reserved raw string delimiter )SKILL\"" >&2
    exit 1
  fi

  printf '    {\n'
  printf '        %s,\n' "$(printf '%s' "$name" | cpp_string_literal)"
  printf '        %s,\n' "$(printf '%s' "$description" | cpp_string_literal)"
  printf '        R"SKILL('
  # Drop the metadata header and one final trailing newline so the generated
  # constants match the old inline literals exactly.
  sed '1,3d' "$path" | awk '
    { lines[++n] = $0 }
    END {
      for (i = 1; i <= n; ++i) {
        printf "%s", lines[i]
        if (i < n) printf "\n"
      }
    }
  '
  printf ')SKILL",\n'
  printf '    },\n'
}

names_file="$out.names"
: > "$names_file"
trap 'rm -f "$names_file"' EXIT HUP INT TERM
for skill in "$@"; do
  name_line=$(sed -n '1p' "$skill")
  case "$name_line" in
    "# Name: "*) printf '%s\n' "${name_line#"# Name: "}" >> "$names_file" ;;
    *) echo "$skill: first line must be '# Name: <skill-name>'" >&2; exit 1 ;;
  esac
done
if [ "$(sort "$names_file" | uniq -d | wc -l | tr -d ' ')" != "0" ]; then
  echo "duplicate built-in skill name" >&2
  exit 1
fi

{
  cat <<'HEADER'
#ifndef CORE_BUILTIN_SKILLS_DATA_H_
#define CORE_BUILTIN_SKILLS_DATA_H_

#include <cstddef>

namespace slop {

struct BuiltinSkillData {
  const char* name;
  const char* description;
  const char* system_prompt_patch;
};

inline constexpr BuiltinSkillData kBuiltinSkills[] = {
HEADER
  for skill in "$@"; do
    emit_skill "$skill"
  done
  cat <<'FOOTER'
};

inline constexpr std::size_t kBuiltinSkillCount =
    sizeof(kBuiltinSkills) / sizeof(kBuiltinSkills[0]);

}  // namespace slop

#endif  // CORE_BUILTIN_SKILLS_DATA_H_
FOOTER
} > "$out"
