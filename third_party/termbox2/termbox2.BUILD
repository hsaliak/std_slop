load("@rules_cc//cc:defs.bzl", "cc_library")

# Generate the implementation file from the header.
# termbox2 is a single-header library that requires TB_IMPL to be defined in
# exactly one translation unit to emit the implementation code.
genrule(
    name = "gen_termbox2_impl",
    srcs = ["termbox2.h"],
    outs = ["termbox2.c"],
    cmd = "echo '#define TB_IMPL' > $@ && cat $< >> $@",
)

cc_library(
    name = "termbox2",
    srcs = [":gen_termbox2_impl"],
    hdrs = ["termbox2.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)
