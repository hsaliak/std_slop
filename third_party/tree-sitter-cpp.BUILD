cc_library(
    name = "tree-sitter-cpp",
    srcs = [
        "src/parser.c",
        "src/scanner.c",
    ],
    hdrs = glob(["src/**/*.h"]),
    includes = ["src"],
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
