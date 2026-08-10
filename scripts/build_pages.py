#!/usr/bin/env python3
"""Build the static Pages content from the repository's Markdown sources.

The site wrappers, navigation, and stylesheet are site-specific. Page prose and
code examples are selected from README.md, docs/, and markdown/README.md so the
repository documentation remains the content authority.
"""

from __future__ import annotations

import argparse
import html
import posixpath
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

REPOSITORY_URL = "https://github.com/hsaliak/std_slop"


@dataclass(frozen=True)
class SourceSpec:
    path: str
    sections: tuple[str, ...] | None = None
    include_intro: bool = False


@dataclass(frozen=True)
class PageSpec:
    output: str
    label: str
    title: str
    sources: tuple[SourceSpec, ...]
    show_hero: bool = True


PAGES = (
    PageSpec(
        "index.html",
        "README.md",
        "std::slop",
        (SourceSpec("README.md", (
            "Key Features",
            "🚀 Quick Start",
            "Authentication Quick Notes",
            "⚙️ Configuration",
            "💻 Code",
            "📚 Documentation",
            "🏗️ Architecture & Codebase Layout",
        ), True),),
        False,
    ),
    PageSpec(
        "agent.html",
        "README.md + docs/",
        "Agent runtime",
        (
            SourceSpec("README.md", ("Key Features", "🚀 Quick Start", "Authentication Quick Notes", "⚙️ Configuration")),
            SourceSpec("docs/WALKTHROUGH.md"),
            SourceSpec("docs/SESSIONS.md"),
            SourceSpec("docs/CONTEXT_MANAGEMENT.md"),
            SourceSpec("docs/mail_mode.md"),
        ),
    ),
    PageSpec(
        "mcp.html",
        "docs/mcp-api.md + docs/mcp-slop-userguide.md",
        "MCP library and runtime",
        (SourceSpec("docs/mcp-api.md"), SourceSpec("docs/mcp-slop-userguide.md")),
    ),
    PageSpec(
        "markdown.html",
        "markdown/README.md",
        "Markdown library",
        (SourceSpec("markdown/README.md"),),
    ),
    PageSpec(
        "docs.html",
        "docs/README.md",
        "Documentation guide",
        (SourceSpec("docs/README.md"),),
    ),
)

NAVIGATION = (
    ("Agent", "agent.html"),
    ("MCP", "mcp.html"),
    ("Markdown", "markdown.html"),
    ("Docs", "docs.html"),
    ("GitHub", f"{REPOSITORY_URL}"),
)


def heading(line: str) -> tuple[int, str] | None:
    match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
    if not match:
        return None
    return len(match.group(1)), match.group(2)


def normalize_heading(text: str) -> str:
    return re.sub(r"\s+", " ", text.strip())


def heading_positions(lines: list[str]) -> dict[int, tuple[int, str]]:
    positions: dict[int, tuple[int, str]] = {}
    in_fence = False
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            current = heading(line)
            if current:
                positions[index] = current
    return positions


def extract_sections(lines: list[str], wanted: tuple[str, ...] | None) -> list[str]:
    if wanted is None:
        return lines
    positions = heading_positions(lines)
    wanted_set = {normalize_heading(item) for item in wanted}
    result: list[str] = []
    index = 0
    while index < len(lines):
        current = positions.get(index)
        if current is None or normalize_heading(current[1]) not in wanted_set:
            index += 1
            continue
        level = current[0]
        end = index + 1
        while end < len(lines):
            candidate = positions.get(end)
            if candidate is not None and candidate[0] <= level:
                break
            end += 1
        result.extend(lines[index:end])
        index = end
    return result


def read_source(root: Path, spec: SourceSpec) -> list[str]:
    lines = (root / spec.path).read_text(encoding="utf-8").splitlines()
    if spec.sections is None:
        return lines
    selected = extract_sections(lines, spec.sections)
    if not spec.include_intro:
        return selected

    wanted_set = {normalize_heading(item) for item in spec.sections}
    positions = heading_positions(lines)
    first_section = next(
        (i for i, current in positions.items() if normalize_heading(current[1]) in wanted_set),
        len(lines),
    )
    intro = lines[:first_section]
    intro = [line for line in intro if not line.startswith("# ") and not line.startswith("[")]
    return intro + selected


def source_url(source_path: str, target: str) -> str:
    if target.startswith(("http://", "https://", "mailto:", "#")):
        return target
    parsed = urlsplit(target)
    path = parsed.path
    fragment = f"#{parsed.fragment}" if parsed.fragment else ""
    if path.startswith("/"):
        return target
    source_dir = Path(source_path).parent
    resolved = posixpath.normpath((source_dir / path).as_posix())
    if resolved.startswith("../"):
        resolved = resolved[3:]
    if resolved.endswith(".md") or resolved.endswith(".ini"):
        return f"{REPOSITORY_URL}/blob/main/{resolved}{fragment}"
    return target


def inline(text: str, source_path: str) -> str:
    tokens: list[str] = []

    def token(value: str) -> str:
        tokens.append(value)
        return f"\x00{len(tokens) - 1}\x00"

    value = html.escape(text, quote=False)
    value = re.sub(
        r"!\[([^\]]*)\]\(([^)]+)\)",
        lambda match: token(
            f'<img src="{html.escape(image_path(match.group(2), source_path), quote=True)}" alt="{html.escape(match.group(1), quote=True)}">'
        ),
        value,
    )
    value = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        lambda match: token(
            f'<a href="{html.escape(source_url(source_path, match.group(2)), quote=True)}">{match.group(1)}</a>'
        ),
        value,
    )
    value = re.sub(r"`([^`]+)`", lambda match: token(f"<code>{match.group(1)}</code>"), value)
    value = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", value)
    value = re.sub(r"\*([^*]+)\*", r"<em>\1</em>", value)
    for index, replacement in enumerate(tokens):
        value = value.replace(f"\x00{index}\x00", replacement)
    return value


def image_path(target: str, source_path: str) -> str:
    resolved = posixpath.normpath((Path(source_path).parent / target).as_posix())
    if resolved == "docs/slop.png":
        return "assets/slop.png"
    if resolved == "docs/mail_model.png":
        return "assets/mail_model.png"
    return target


def table(lines: list[str], source_path: str) -> str:
    rows = []
    for line in lines:
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        rows.append(cells)
    header, body = rows[0], rows[2:]
    output = ["<div class=\"table-wrap\"><table><thead><tr>"]
    output.extend(f"<th>{inline(cell, source_path)}</th>" for cell in header)
    output.append("</tr></thead><tbody>")
    for row in body:
        output.append("<tr>")
        output.extend(f"<td>{inline(cell, source_path)}</td>" for cell in row)
        output.append("</tr>")
    output.extend(["</tbody></table></div>"])
    return "".join(output)


def render_markdown(lines: list[str], source_path: str) -> str:
    output: list[str] = []
    paragraph: list[str] = []
    index = 0

    def flush_paragraph() -> None:
        if paragraph:
            output.append(f"<p>{inline(' '.join(paragraph), source_path)}</p>")
            paragraph.clear()

    while index < len(lines):
        line = lines[index]
        stripped = line.strip()
        if not stripped:
            flush_paragraph()
            index += 1
            continue
        if stripped.startswith("[!["):
            index += 1
            continue
        if stripped.startswith("```"):
            flush_paragraph()
            language = stripped[3:].strip()
            index += 1
            code: list[str] = []
            while index < len(lines) and not lines[index].strip().startswith("```"):
                code.append(lines[index])
                index += 1
            if index < len(lines):
                index += 1
            class_attr = f' class="language-{html.escape(language, quote=True)}"' if language else ""
            output.append(f"<pre><code{class_attr}>{html.escape(chr(10).join(line.rstrip() for line in code))}</code></pre>")
            continue
        current_heading = heading(line)
        if current_heading:
            flush_paragraph()
            level, title = current_heading
            output.append(f'<h{level}>{inline(title, source_path)}</h{level}>')
            index += 1
            continue
        if stripped.startswith("|") and index + 1 < len(lines) and re.match(r"^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)+\|?\s*$", lines[index + 1]):
            flush_paragraph()
            table_lines = [line, lines[index + 1]]
            index += 2
            while index < len(lines) and lines[index].strip().startswith("|"):
                table_lines.append(lines[index])
                index += 1
            output.append(table(table_lines, source_path))
            continue
        list_match = re.match(r"^\s*([-*])\s+(.+)$", line)
        ordered_match = re.match(r"^\s*\d+[.)]\s+(.+)$", line)
        if list_match or ordered_match:
            flush_paragraph()
            ordered = ordered_match is not None
            tag = "ol" if ordered else "ul"
            items: list[str] = []
            while index < len(lines):
                match = re.match(r"^\s*\d+[.)]\s+(.+)$", lines[index]) if ordered else re.match(r"^\s*[-*]\s+(.+)$", lines[index])
                if not match:
                    break
                items.append(f"<li>{inline(match.group(1), source_path)}</li>")
                index += 1
            output.append(f"<{tag}>" + "".join(items) + f"</{tag}>")
            continue
        if stripped.startswith(">"):
            flush_paragraph()
            output.append(f"<blockquote>{inline(stripped[1:].strip(), source_path)}</blockquote>")
            index += 1
            continue
        paragraph.append(stripped)
        index += 1
    flush_paragraph()
    return "\n".join(output)


def navigation(active: str) -> str:
    links = []
    for label, target in NAVIGATION:
        current = ' class="active" aria-current="page"' if target == active else ""
        links.append(f'<a{current} href="{target}">{label}</a>')
    return "".join(links)


def page_document(spec: PageSpec, content: str) -> str:
    title_markup = "" if content.lstrip().startswith("<h1>") else f"<h1>{html.escape(spec.title)}</h1>"
    hero = f'<section class="page-hero shell"><p class="eyebrow">Source: {html.escape(spec.label)}</p>{title_markup}</section>' if title_markup and spec.show_hero else ""
    return f'''<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="description" content="Technical documentation for std::slop: a C++ coding agent and integrated MCP and Markdown libraries.">
  <title>{html.escape(spec.title)} — std::slop</title>
  <link rel="stylesheet" href="styles.css">
</head>
<body>
  <a class="skip-link" href="#main-content">Skip to content</a>
  <header class="site-header"><div class="shell nav-row"><a class="brand" href="index.html" aria-label="std::slop home"><img src="assets/slop.png" alt="std::slop logo"></a><nav aria-label="Primary navigation">{navigation(spec.output)}</nav></div></header>
  <main id="main-content">
{hero}
    <article class="shell markdown-content">{content}<p class="source-note">Content is generated from the repository sources listed above. See the source files for the complete reference.</p></article>
  </main>
  <footer class="site-footer"><div class="shell footer-row"><span><span class="prompt">$</span> std::slop</span><span>C++ coding agent · MCP and Markdown libraries</span><a href="{REPOSITORY_URL}">Source on GitHub</a></div></footer>
</body>
</html>
'''


def write_not_found(output: Path) -> None:
    (output / "404.html").write_text(page_document(PageSpec("404.html", "site", "Page not found", ()), '<p>The requested page does not exist.</p><p><a href="index.html">Return to the documentation index.</a></p>'), encoding="utf-8")


def build(root: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    for old in output.glob("*.html"):
        old.unlink()
    for spec in PAGES:
        fragments = []
        for source in spec.sources:
            fragments.append(render_markdown(read_source(root, source), source.path))
        (output / spec.output).write_text(page_document(spec, "\n".join(fragments)), encoding="utf-8")
    write_not_found(output)
    stylesheet = root / "site/styles.css"
    if stylesheet.resolve() != (output / "styles.css").resolve():
        shutil.copy2(stylesheet, output / "styles.css")
    assets = output / "assets"
    assets.mkdir(exist_ok=True)
    for source_name in ("slop.png", "mail_model.png"):
        source = root / "docs" / source_name
        target = assets / source_name
        if source.resolve() != target.resolve():
            shutil.copy2(source, target)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("site"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build(root, (root / args.output).resolve() if not args.output.is_absolute() else args.output)


if __name__ == "__main__":
    main()
