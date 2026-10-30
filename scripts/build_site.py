# /// script
# dependencies = ["markdown-it-py==3.0.0"]
# ///
"""Build the GitHub Pages site from docs/ and README.md.

Usage:
    python3 build_site.py --out DIR

Copies the static assets in docs/site/ and renders each page in PAGES into docs/templates/page.html. Pages come
from an HTML fragment, a markdown file, or a list of README sections, so the site and the README can't drift
apart. Links between README sections (#duckdb-wasm) are rewritten to the page the section ends up on.
"""

import argparse
import html
import re
import shutil
from pathlib import Path
from string import Template

from markdown_it import MarkdownIt

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'

# (output directory, title, source). A README source is a list of section headings: each is included with its
# subsections. The first heading is dropped when it names the page itself, so its body becomes the page intro.
PAGES = [
    ('', 'DuckDB GSheets', DOCS / 'templates' / 'home.html'),
    ('install', 'Install', ['Install']),
    (
        'authentication',
        'Authenticate',
        [
            'Authenticate',
            'How the browser login works',
            'Pipelines and headless machines',
            'Getting a Google API Access Token',
            'Getting a Google API Access Private Key',
        ],
    ),
    ('read-and-write', 'Read and write', ['Read', 'Write', 'HTTP Proxy']),
    ('limitations', 'Limitations / Known Issues', ['Limitations / Known Issues', 'Support']),
    ('privacy', 'Privacy Policy', DOCS / 'privacy.md'),
]

# Pages linked from the header, in order, with their link text
NAV = [
    ('install', 'Install'),
    ('authentication', 'Authenticate'),
    ('read-and-write', 'Read and write'),
    ('limitations', 'Limitations'),
]

FENCE = re.compile(r'^\s*(```|~~~)')
HEADING = re.compile(r'^(#{2,6}) (.+?)\s*$')


def slugify(title):
    """GitHub's heading anchor, so README links keep working on the site."""
    return re.sub(r'[^\w\- ]', '', title.lower()).replace(' ', '-')


def parse_sections(markdown):
    """Split markdown into [level, title, body lines] at each heading outside a code block."""
    sections = [[1, None, []]]
    in_fence = False
    for line in markdown.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
        heading = None if in_fence else HEADING.match(line)
        if heading:
            sections.append([len(heading.group(1)), heading.group(2), []])
        else:
            sections[-1][2].append(line)
    return sections


def select(sections, title, page_title):
    """Markdown for the section named title and its subsections, with the top heading rendered as an h2."""
    index = next((i for i, s in enumerate(sections) if s[1] == title), None)
    if index is None:
        raise SystemExit(f'README.md has no section named {title!r}')
    level = sections[index][0]
    end = next((i for i in range(index + 1, len(sections)) if sections[i][0] <= level), len(sections))
    intro = title == page_title
    shift = level - (1 if intro else 2)
    lines = []
    for depth, heading, body in sections[index:end]:
        if not (intro and heading == title):
            lines.append(f'{"#" * (depth - shift)} {heading}')
        lines.extend(body)
    return '\n'.join(lines), [s[1] for s in sections[index:end]]


def add_heading_ids(rendered):
    def add_id(match):
        text = html.unescape(re.sub(r'<[^>]+>', '', match.group(2)))
        return f'<h{match.group(1)} id="{slugify(text)}">{match.group(2)}</h{match.group(1)}>'

    return re.sub(r'<h([2-6])>(.*?)</h\1>', add_id, rendered)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path, help='output directory, replaced if it exists')
    args = parser.parse_args()

    readme = parse_sections((ROOT / 'README.md').read_text())
    md = MarkdownIt('commonmark', {'html': True}).enable('table')
    template = Template((DOCS / 'templates' / 'page.html').read_text())

    # Build every page's markdown first, so links can point at sections on any page
    sources = {}
    anchors = {}
    for path, title, source in PAGES:
        if isinstance(source, list):
            parts = []
            for heading in source:
                text, headings = select(readme, heading, title)
                parts.append(text)
                for h in headings:
                    anchors[slugify(h)] = f'{path}/' if h == title else f'{path}/#{slugify(h)}'
            sources[path] = '\n\n'.join(parts)
        else:
            sources[path] = source

    if args.out.exists():
        shutil.rmtree(args.out)
    shutil.copytree(DOCS / 'site', args.out)

    for path, title, _ in PAGES:
        root = '../' * (path.count('/') + 1) if path else ''
        source = sources[path]
        if isinstance(source, Path) and source.suffix == '.html':
            content = source.read_text()
        else:
            text = source.read_text() if isinstance(source, Path) else f'# {title}\n\n{source}'

            def link(match):
                target = anchors.get(match.group(1))
                if target is None:
                    raise SystemExit(f'{path or "index"}: no section for link #{match.group(1)}')
                same_page = target.split('#')[0] == f'{path}/'
                return f'](#{match.group(1)})' if same_page else f']({root}{target})'

            text = re.sub(r'\]\(#([\w-]+)\)', link, text)
            content = add_heading_ids(md.render(text))

        current = ' aria-current="page"'
        nav = '\n'.join(
            f'      <a href="{root}{p}/"{current if p == path else ""}>{label}</a>' for p, label in NAV
        )
        out = args.out / path / 'index.html'
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            template.substitute(
                title=title if not path else f'{title} · DuckDB GSheets', root=root, nav=nav, content=content
            )
        )
        print(f'Wrote {out}')


if __name__ == '__main__':
    main()
