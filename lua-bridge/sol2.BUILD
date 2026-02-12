load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "sol2",
    hdrs = glob(["include/**/*.hpp", "include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
    deps = ["@lua//:lua"],
)
