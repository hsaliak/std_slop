def _generate_js_tools_header_impl(ctx):
    manifest = ctx.file.manifest
    js_files = ctx.files.js_files
    out = ctx.outputs.out

    ctx.actions.run_shell(
        inputs = [manifest] + js_files,
        outputs = [out],
        arguments = [manifest.path, out.path],
        command = """set -euo pipefail
MANIFEST="$1"
OUT="$2"
python3 - "$MANIFEST" "$OUT" <<'PY'
import json
import os
import sys


def main():
    manifest_path = sys.argv[1]
    output_path = sys.argv[2]
    js_tools_dir = os.path.dirname(manifest_path)

    with open(manifest_path, "r") as f:
        manifest = json.load(f)

    with open(output_path, "w") as f:
        f.write('''#pragma once

#include <string>
#include <vector>

namespace slop {

struct DefaultJsFunction {
  std::string name;
  std::string description;
  std::string json_schema;
  std::string code;
};

inline const std::vector<DefaultJsFunction>& GetDefaultJsFunctions() {
  static const std::vector<DefaultJsFunction> functions = {
''')

        for tool in manifest:
            name = tool["name"]
            description = tool["description"]
            schema = tool["json_schema"]
            schema_str = json.dumps(schema) if isinstance(schema, dict) else schema

            # Manifest-defined native tools may intentionally omit an implementation file.
            # For these entries, we persist an empty code blob while still preserving
            # name/description/schema so tools.help() stays deterministic from manifest data.
            code = ""
            implementation = tool.get("implementation")
            if implementation:
                js_file = os.path.join(js_tools_dir, implementation)
                with open(js_file, "r") as js_f:
                    code = js_f.read()

            print("    {", file=f)
            print(f'      "{name}",', file=f)
            print(f'      R"SLOP({description})SLOP",', file=f)
            print(f'      R"SLOP({schema_str})SLOP",', file=f)
            print(f'      R"SLOP({code})SLOP"', file=f)
            print("    },", file=f)

        f.write('''  };
  return functions;
}

}  // namespace slop
''')


if __name__ == "__main__":
    main()
PY
""",
    )

    return DefaultInfo(files = depset([out]))


generate_js_tools_header = rule(
    implementation = _generate_js_tools_header_impl,
    attrs = {
        "manifest": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "js_files": attr.label_list(
            allow_files = [".js"],
            mandatory = True,
        ),
        "out": attr.output(mandatory = True),
    },
)

