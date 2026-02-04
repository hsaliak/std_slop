cc_library(
    name = "tree-sitter-go",
    srcs = [
        "src/parser.c",
    ],
    hdrs = glob(["src/**/*.h"]),
    includes = ["src"],
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
