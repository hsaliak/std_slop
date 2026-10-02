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
    "docs/mcp-api.md": (
        "# MCP client API\n\nOutbound HTTP client.\n\n"
        "## Scope\n\nHTTP scope.\n\n"
        "## Connect with automatic protocol selection\n\nConnect example.\n\n"
        "## Bearer token clients\n\nBearer example.\n\n"
        "## Error model\n\nClient errors.\n\n"
        "## Security notes\n\nClient security.\n"
    ),
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

    def test_agent_interfaces_have_separate_concise_pages(self):
        agent = self.page("agent.html")
        self.assertIn("Interactive setup", agent)
        for text in ("Agent workflow details", "Agent sessions",
                     "Agent context", "Patch review",
                     "Scripted prompts and state commands"):
            self.assertNotIn(text, agent)
        self.assertIn("Scripted prompts and state commands",
                      self.page("sl.html"))

    def test_client_and_server_have_separate_pages(self):
        client = self.page("mcp.html")
        self.assertIn("Outbound HTTP client", client)
        self.assertIn("Connect example", client)
        self.assertNotIn("Inbound stdio server", client)
        self.assertNotIn("Remote tools", client)
        server = self.page("mcp-server.html")
        for text in ("Inbound stdio server", "2026-07-28",
                     "//mcp/server:echo_server"):
            self.assertIn(text, server)
        self.assertNotIn("Outbound HTTP client", server)

    def test_each_component_has_exactly_one_markdown_source(self):
        expected = {
            "agent.html": "docs/WALKTHROUGH.md",
            "sl.html": "docs/sl.md",
            "mcp.html": "docs/mcp-api.md",
            "mcp-server.html": "docs/mcp-server.md",
            "markdown.html": "markdown/README.md",
        }
        for page in build_pages.PAGES:
            if page.output in expected:
                self.assertEqual(len(page.sources), 1)
                self.assertEqual(page.sources[0].path, expected[page.output])

    def test_navigation_order_and_agent_dropdown(self):
        self.assertEqual(
            [name for name, _ in build_pages.NAVIGATION],
            ["Coding Agents", "MCP Client", "MCP Server", "Markdown"],
        )
        for name in ("agent.html", "sl.html"):
            navigation = build_pages.navigation(name)
            self.assertIn('<summary class="active">Coding Agents', navigation)
            self.assertIn(f'aria-current="page" href="{name}"', navigation)
            self.assertEqual(navigation.count('aria-current="page"'), 1)

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
                self.assertIn('href="agent.html">std::slop', page)
                self.assertIn('href="sl.html">sl', page)
                self.assertIn('href="mcp-server.html"', page)
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
