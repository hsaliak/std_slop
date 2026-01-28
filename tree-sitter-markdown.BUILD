cc_library(
    name = "tree-sitter-markdown",
    srcs = [
        "tree-sitter-markdown/src/parser.c",
        "tree-sitter-markdown/src/scanner.c",
    ],
    hdrs = [
        "tree-sitter-markdown/src/tree_sitter/parser.h",
    ],
    includes = [
        "tree-sitter-markdown/src",
    ],
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)

cc_library(
    name = "tree-sitter-markdown-inline",
    srcs = [
        "tree-sitter-markdown-inline/src/parser.c",
        "tree-sitter-markdown-inline/src/scanner.c",
    ],
    hdrs = [
        "tree-sitter-markdown-inline/src/tree_sitter/parser.h",
    ],
    includes = [
        "tree-sitter-markdown-inline/src",
    ],
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)

