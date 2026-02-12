load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _sol2_extension_impl(module_ctx):
    http_archive(
        name = "sol2",
        build_file = "//lua-bridge:sol2.BUILD",
        sha256 = "b82c5de030e18cb2bcbcefcd5f45afd526920c517a96413f0b59b4332d752a1e",
        strip_prefix = "sol2-3.3.0",
        urls = ["https://github.com/ThePhD/sol2/archive/refs/tags/v3.3.0.tar.gz"],
    )

sol2 = module_extension(
    implementation = _sol2_extension_impl,
)
