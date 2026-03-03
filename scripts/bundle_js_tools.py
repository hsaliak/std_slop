import json
import os
import sys

def main():
    manifest_path = sys.argv[1]
    output_path = sys.argv[2]
    js_tools_dir = os.path.dirname(manifest_path)

    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    with open(output_path, 'w') as f:
        f.write("#pragma once\n\n")
        f.write("#include <string>\n")
        f.write("#include <vector>\n\n")
        f.write("namespace slop {\n\n")
        f.write("struct DefaultJsFunction {\n")
        f.write("  std::string name;\n")
        f.write("  std::string description;\n")
        f.write("  std::string json_schema;\n")
        f.write("  std::string code;\n")
        f.write("};\n\n")
        f.write("inline const std::vector<DefaultJsFunction>& GetDefaultJsFunctions() {\n")
        f.write("  static const std::vector<DefaultJsFunction> functions = {\n")

        for tool in manifest:
            name = tool['name']
            description = tool['description']
            schema = tool['json_schema']
            if isinstance(schema, dict):
                schema_str = json.dumps(schema)
            else:
                schema_str = schema
            
            js_file = os.path.join(js_tools_dir, tool['implementation'])
            with open(js_file, 'r') as js_f:
                code = js_f.read()

            f.write("    {\n")
            f.write(f'      "{name}",\n')
            f.write(f'      R"SLOP({description})SLOP",\n')
            f.write(f'      R"SLOP({schema_str})SLOP",\n')
            f.write(f'      R"SLOP({code})SLOP"\n')
            f.write("    },\n")

        f.write("  };\n")
        f.write("  return functions;\n")
        f.write("}\n\n")
        f.write("}  // namespace slop\n")

if __name__ == "__main__":
    main()

