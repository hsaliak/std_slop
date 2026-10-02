"""Tests for component-first documentation page selection and rendering."""

import os
from pathlib import Path
import tempfile
import unittest

import build_pages


README = """# std::slop

A C++ monorepo for agentic tooling.

## Components

Markdown library, MCP client, MCP server, `std_slop`, and `sl`.

## Library quick starts

Library build examples.

## Component boundaries

The Markdown renderer produces terminal output, not HTML.

## Agent entry points

Two coding-agent interfaces.

## Agent features

Agent workflow details.

## Agent quick start

Agent setup details.

## Build and test

Monorepo build commands.

## Code conventions

C++17 conventions.

## Documentation

[sl guide](docs/sl.md)

## Repository layout

Package map.
"""

SOURCES = {
    "README.md": README,
    "docs/WALKTHROUGH.md": (
        "# Coding-agent walkthrough\n\nInteractive setup.\n"
    ),
    "docs/sl.md": (
        "# sl command-line guide\n\nScripted prompts and state commands.\n"
    ),
    "docs/SESSIONS.md": "# Sessions\n\nAgent sessions.\n",
    "docs/CONTEXT_MANAGEMENT.md": "# Context management\n\nAgent context.\n",
    "docs/mail_mode.md": "# Mail mode\n\nPatch review.\n",
    "docs/mcp-api.md": "# MCP client API\n\nOutbound HTTP client.\n",
    "docs/mcp-server.md": (
        "# MCP stdio server\n\n"
        "Inbound stdio server, protocol `2026-07-28`.\n\n"
        "```sh\nbazel build //mcp/server:echo_server\n```\n"
    ),
    "docs/mcp-slop-userguide.md": "# MCP agent integration\n\nRemote tools.\n",
    "markdown/README.md": "# Markdown library\n\nTerminal renderer.\n",
    "docs/README.md": "# Documentation guide\n\nChoose a component.\n",
}


class BuildPagesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repo"
        self.output = Path(temporary.name) / "output"
        for name, text in SOURCES.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        stylesheet = self.root / "site/styles.css"
        stylesheet.parent.mkdir(parents=True, exist_ok=True)
        stylesheet.write_text("body { color: black; }", encoding="utf-8")
        for name in ("slop.png", "mail_model.png"):
            (self.root / "docs" / name).write_bytes(b"fixture image")
        build_pages.build(self.root, self.output)

    def page(self, name):
        return (self.output / name).read_text(encoding="utf-8")

    def test_homepage_is_a_component_map(self):
        home = self.page("index.html")
        for text in ("C++ monorepo for agentic tooling", "Markdown library",
                     "MCP client", "MCP server", "Library build examples",
                     "Two coding-agent interfaces", "Package map"):
            self.assertIn(text, home)
        self.assertIn("<code>std_slop</code>", home)
        self.assertIn("<code>sl</code>", home)
        self.assertNotIn("Agent workflow details", home)
        self.assertNotIn("Agent setup details", home)

    def test_agent_page_keeps_both_interfaces_and_runtime_details(self):
        agent = self.page("agent.html")
        for text in ("Coding-agent interfaces", "Agent workflow details",
                     "Agent setup details", "Interactive setup",
                     "Scripted prompts and state commands", "Agent sessions",
                     "Agent context", "Patch review"):
            self.assertIn(text, agent)

    def test_mcp_page_includes_client_server_and_agent_integration(self):
        mcp = self.page("mcp.html")
        for text in ("MCP client and server", "Outbound HTTP client",
                     "Inbound stdio server", "2026-07-28",
                     "//mcp/server:echo_server", "Remote tools"):
            self.assertIn(text, mcp)

    def test_selected_sections_exist_in_repository_sources(self):
        runfiles = os.environ.get("TEST_SRCDIR")
        root = (Path(runfiles) / os.environ["TEST_WORKSPACE"] if runfiles
                else Path(__file__).resolve().parent.parent)
        for page in build_pages.PAGES:
            for source in page.sources:
                if source.sections is None:
                    continue
                lines = (root / source.path).read_text().splitlines()
                positions = build_pages.heading_positions(lines)
                headings = {
                    build_pages.normalize_heading(title)
                    for _, title in positions.values()
                }
                for section in source.sections:
                    with self.subTest(page=page.output, section=section):
                        self.assertIn(section, headings)

    def test_all_pages_use_monorepo_framing_and_navigation(self):
        names = {page.output for page in build_pages.PAGES} | {"404.html"}
        self.assertEqual({path.name for path in self.output.glob("*.html")},
                         names)
        for name in names:
            with self.subTest(page=name):
                page = self.page(name)
                self.assertIn("C++ monorepo for agentic tooling", page)
                self.assertIn(
                    "C++ libraries and coding-agent interfaces", page,
                )
                self.assertIn('href="markdown.html"', page)
                self.assertIn('href="mcp.html"', page)
                self.assertIn('href="agent.html">Coding agents', page)
                self.assertNotIn("C++ coding agent and integrated", page)

    def test_relative_guide_links_keep_the_correct_source_path(self):
        self.assertIn(
            'href="https://github.com/hsaliak/std_slop/blob/main/docs/sl.md"',
            self.page("index.html"),
        )
        self.assertEqual(
            build_pages.source_url("docs/README.md", "../markdown/README.md"),
            "https://github.com/hsaliak/std_slop/blob/main/markdown/README.md",
        )


if __name__ == "__main__":
    unittest.main()
