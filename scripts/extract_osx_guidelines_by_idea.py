#!/usr/bin/env python3
"""Export Apple Human Interface Guidelines into idea-organized markdown and photos."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PDF = PROJECT_ROOT / "snowleopardaura" / "OSXGuidelines" / "OSXGuidelines.pdf"
DEFAULT_OUTPUT = PROJECT_ROOT / "snowleopardaura" / "OSXGuidelines" / "ai_markdown_by_idea"

CHAR_REPLACEMENTS = {
    "\u00a0": " ",
    "\u00ad": "",
    "\u00b0": " degrees ",
    "\u00d7": "x",
    "\u00a9": "(c)",
    "\u00ae": "",
    "\u2122": "",
    "\u2010": "-",
    "\u2011": "-",
    "\u2012": "-",
    "\u2013": "-",
    "\u2014": "-",
    "\u2015": "-",
    "\u2018": "'",
    "\u2019": "'",
    "\u201c": '"',
    "\u201d": '"',
    "\u2022": "- ",
    "\u2026": "...",
    "\u2192": "->",
}


@dataclass
class Part:
    roman: str
    title: str
    start_page: int
    slug: str
    order: int


@dataclass
class Chapter:
    number: int
    title: str
    start_page: int
    end_page: int
    slug: str
    part_slug: str
    part_title: str


@dataclass
class Group:
    kind: str
    title: str
    slug: str
    start_page: int
    end_page: int
    folder: Path
    markdown_path: Path
    idea_slug: str | None
    part_title: str | None = None
    chapter_number: int | None = None
    chapter_title: str | None = None

    @property
    def pages(self) -> list[int]:
        return list(range(self.start_page, self.end_page + 1))

    @property
    def group_id(self) -> str:
        prefix = self.idea_slug if self.idea_slug is not None else "front_matter"
        return f"{prefix}::{self.slug}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert the Apple Human Interface Guidelines PDF into idea-organized markdown."
    )
    parser.add_argument("--pdf", type=Path, default=DEFAULT_PDF, help="Source PDF path.")
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="Output folder for the organized export.",
    )
    return parser.parse_args()


def ensure_tools() -> None:
    required = ["pdfinfo", "pdftotext", "pdfimages"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(f"Missing required tools: {', '.join(missing)}")


def run_text(*args: str) -> str:
    result = subprocess.run(args, check=True, capture_output=True, text=True)
    return result.stdout


def run_command(*args: str) -> None:
    subprocess.run(args, check=True)


def normalize_ascii(text: str) -> str:
    for source, target in CHAR_REPLACEMENTS.items():
        text = text.replace(source, target)
    text = unicodedata.normalize("NFKD", text)
    text = text.encode("ascii", "ignore").decode("ascii")
    text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\f", "\n")
    return text


def slugify(text: str) -> str:
    text = normalize_ascii(text).lower()
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return re.sub(r"-{2,}", "-", text)


def strip_edge_blanks(lines: list[str]) -> list[str]:
    start = 0
    end = len(lines)
    while start < end and not lines[start].strip():
        start += 1
    while end > start and not lines[end - 1].strip():
        end -= 1
    return lines[start:end]


def page_count(pdf_path: Path) -> int:
    info = run_text("pdfinfo", str(pdf_path))
    match = re.search(r"^Pages:\s+(\d+)$", info, re.MULTILINE)
    if not match:
        raise RuntimeError("Could not read page count from pdfinfo.")
    return int(match.group(1))


def parse_outline(pdf_path: Path) -> tuple[int, list[Part], list[tuple[int, str, int]]]:
    toc_text = normalize_ascii(
        run_text("pdftotext", "-f", "1", "-l", "20", "-layout", str(pdf_path), "-")
    )

    intro_match = re.search(
        r"Introduction\s+Introduction to Apple Human Interface Guidelines\s+(\d+)",
        " ".join(toc_text.split()),
    )
    if not intro_match:
        raise RuntimeError("Could not determine the Introduction start page.")
    intro_start = int(intro_match.group(1))

    parts: list[Part] = []
    chapters: list[tuple[int, str, int]] = []
    seen_parts: set[int] = set()
    seen_chapters: set[int] = set()

    for raw_line in toc_text.splitlines():
        line = " ".join(raw_line.split())
        part_match = re.match(r"^Part\s+([IVXLC]+)\s+(.+?)\s+(\d+)$", line)
        if part_match:
            start_page = int(part_match.group(3))
            if start_page not in seen_parts:
                seen_parts.add(start_page)
                title = part_match.group(2).strip()
                parts.append(
                    Part(
                        roman=part_match.group(1),
                        title=title,
                        start_page=start_page,
                        slug=f"{len(parts) + 1:02d}-{slugify(title)}",
                        order=len(parts) + 1,
                    )
                )
            continue

        chapter_match = re.match(r"^Chapter\s+(\d+)\s+(.+?)\s+(\d+)$", line)
        if chapter_match:
            start_page = int(chapter_match.group(3))
            if start_page not in seen_chapters:
                seen_chapters.add(start_page)
                chapters.append(
                    (
                        int(chapter_match.group(1)),
                        chapter_match.group(2).strip(),
                        start_page,
                    )
                )

    chapters.sort(key=lambda item: item[2])
    parts.sort(key=lambda item: item.start_page)
    return intro_start, parts, chapters


def cleanup_page_text(
    raw_text: str,
    page_number: int,
    intro_start: int,
    part_starts: set[int],
    chapter_starts: set[int],
) -> str:
    lines = [line.rstrip() for line in normalize_ascii(raw_text).splitlines()]
    lines = strip_edge_blanks(lines)

    cleaned: list[str] = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            if cleaned and cleaned[-1] != "":
                cleaned.append("")
            continue
        if stripped.startswith("2008-06-09 |"):
            continue
        if stripped.startswith("(c) 1992, 2001-2003, 2008 Apple Inc."):
            continue
        if re.fullmatch(r"\d+", stripped):
            continue
        if stripped.startswith(f"{page_number} ") and re.search(r"[A-Za-z]", stripped):
            continue
        if stripped.endswith(f" {page_number}") and re.search(r"[A-Za-z]", stripped):
            continue
        cleaned.append(line)

    cleaned = strip_edge_blanks(cleaned)
    if not cleaned:
        return ""

    nonempty_indices = [index for index, line in enumerate(cleaned) if line.strip()]
    first_two_positions = nonempty_indices[:2]
    first_two = [cleaned[index].strip() for index in first_two_positions]

    is_intro_start = page_number == intro_start
    is_part_start = page_number in part_starts
    is_chapter_start = page_number in chapter_starts

    def drop_positions(count: int) -> None:
        remaining = []
        dropped = 0
        for line in cleaned:
            if line.strip() and dropped < count:
                dropped += 1
                continue
            remaining.append(line)
        cleaned[:] = strip_edge_blanks(remaining)

    if first_two and not is_intro_start and first_two[0] == "INTRODUCTION":
        drop_positions(min(2, len(first_two)))
    elif first_two and not is_part_start and re.fullmatch(r"PART\s+[IVXLC]+", first_two[0]):
        drop_positions(min(2, len(first_two)))
    elif first_two and not is_chapter_start and re.fullmatch(r"CHAPTER\s+\d+", first_two[0]):
        drop_positions(min(2, len(first_two)))

    cleaned = strip_edge_blanks(cleaned)
    return "\n".join(cleaned).strip()


def collect_page_texts(
    pdf_path: Path,
    total_pages: int,
    intro_start: int,
    part_starts: set[int],
    chapter_starts: set[int],
) -> dict[int, str]:
    pages: dict[int, str] = {}
    for page in range(1, total_pages + 1):
        raw = run_text(
            "pdftotext",
            "-f",
            str(page),
            "-l",
            str(page),
            "-layout",
            str(pdf_path),
            "-",
        )
        pages[page] = cleanup_page_text(raw, page, intro_start, part_starts, chapter_starts)
    return pages


def parse_pdfimages_rows(pdf_path: Path) -> list[dict[str, int | str]]:
    rows: list[dict[str, int | str]] = []
    listing = run_text("pdfimages", "-list", str(pdf_path))
    for line in listing.splitlines()[2:]:
        parts = line.split()
        if len(parts) < 5 or parts[2] not in {"image", "smask"}:
            continue
        rows.append(
            {
                "page": int(parts[0]),
                "object_num": int(parts[1]),
                "type": parts[2],
                "width": int(parts[3]),
                "height": int(parts[4]),
            }
        )
    return rows


def extract_raw_images(pdf_path: Path, temp_dir: Path) -> list[dict[str, object]]:
    rows = parse_pdfimages_rows(pdf_path)
    temp_dir.mkdir(parents=True, exist_ok=True)
    prefix = temp_dir / "image"
    run_command("pdfimages", "-png", str(pdf_path), str(prefix))

    raw_files = sorted(temp_dir.glob("image-*.png"))
    if len(raw_files) != len(rows):
        raise RuntimeError(
            f"Expected {len(rows)} extracted files from pdfimages, found {len(raw_files)}."
        )

    manifest: list[dict[str, object]] = []
    per_page_counter: dict[int, int] = {}
    index = 0
    while index < len(rows):
        row = rows[index]
        raw_path = raw_files[index]
        if row["type"] != "image":
            index += 1
            continue

        page = int(row["page"])
        per_page_counter[page] = per_page_counter.get(page, 0) + 1
        page_index = per_page_counter[page]
        temp_output = temp_dir / f"page-{page:03d}-image-{page_index:02d}.png"

        next_is_mask = (
            index + 1 < len(rows)
            and rows[index + 1]["type"] == "smask"
            and int(rows[index + 1]["page"]) == page
        )
        if next_is_mask:
            with Image.open(raw_path) as base_image, Image.open(raw_files[index + 1]) as mask_image:
                rgba = base_image.convert("RGBA")
                mask = mask_image.convert("L")
                if mask.size == rgba.size:
                    rgba.putalpha(mask)
                rgba.save(temp_output)
            index += 1
        else:
            shutil.copy2(raw_path, temp_output)

        manifest.append(
            {
                "page": page,
                "page_index": page_index,
                "object_num": int(row["object_num"]),
                "width": int(row["width"]),
                "height": int(row["height"]),
                "temp_path": str(temp_output),
            }
        )
        index += 1

    for path in raw_files:
        if path.exists():
            path.unlink()
    return manifest


def reset_output_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def build_chapters(parts: list[Part], chapter_rows: list[tuple[int, str, int]], total_pages: int) -> list[Chapter]:
    chapters: list[Chapter] = []
    for index, (number, title, start_page) in enumerate(chapter_rows):
        end_page = chapter_rows[index + 1][2] - 1 if index + 1 < len(chapter_rows) else total_pages
        current_part = parts[0]
        for part in parts:
            if part.start_page <= start_page:
                current_part = part
        chapters.append(
            Chapter(
                number=number,
                title=title,
                start_page=start_page,
                end_page=end_page,
                slug=f"chapter-{number:02d}-{slugify(title)}",
                part_slug=current_part.slug,
                part_title=current_part.title,
            )
        )
    return chapters


def build_groups(
    output_root: Path,
    intro_start: int,
    parts: list[Part],
    chapters: list[Chapter],
) -> tuple[list[Group], list[Group], list[Group]]:
    front_matter_dir = output_root / "front_matter"
    groups: list[Group] = []

    front_matter_groups = [
        Group(
            kind="front_matter",
            title="Cover and Legal",
            slug="00-cover-and-legal",
            start_page=1,
            end_page=2,
            folder=front_matter_dir,
            markdown_path=front_matter_dir / "markdown" / "00-cover-and-legal.md",
            idea_slug=None,
        ),
        Group(
            kind="front_matter",
            title="Contents",
            slug="01-contents",
            start_page=3,
            end_page=intro_start - 1,
            folder=front_matter_dir,
            markdown_path=front_matter_dir / "markdown" / "01-contents.md",
            idea_slug=None,
        ),
    ]
    groups.extend(front_matter_groups)

    idea_groups: list[Group] = []
    intro_folder = output_root / "ideas" / "00-introduction"
    intro_end = parts[0].start_page - 1
    intro_group = Group(
        kind="idea_overview",
        title="Introduction to Apple Human Interface Guidelines",
        slug="00-introduction",
        start_page=intro_start,
        end_page=intro_end,
        folder=intro_folder,
        markdown_path=intro_folder / "markdown" / "00-introduction.md",
        idea_slug="00-introduction",
        part_title="Introduction",
    )
    groups.append(intro_group)
    idea_groups.append(intro_group)

    chapter_groups: list[Group] = []
    chapters_by_part: dict[str, list[Chapter]] = {}
    for chapter in chapters:
        chapters_by_part.setdefault(chapter.part_slug, []).append(chapter)

    for part in parts:
        folder = output_root / "ideas" / part.slug
        part_chapters = chapters_by_part[part.slug]
        overview_end = part_chapters[0].start_page - 1
        overview_group = Group(
            kind="idea_overview",
            title=part.title,
            slug="00-overview",
            start_page=part.start_page,
            end_page=overview_end,
            folder=folder,
            markdown_path=folder / "markdown" / "00-overview.md",
            idea_slug=part.slug,
            part_title=part.title,
        )
        groups.append(overview_group)
        idea_groups.append(overview_group)

        for chapter in part_chapters:
            chapter_group = Group(
                kind="chapter",
                title=f"Chapter {chapter.number}: {chapter.title}",
                slug=chapter.slug,
                start_page=chapter.start_page,
                end_page=chapter.end_page,
                folder=folder,
                markdown_path=folder / "markdown" / f"{chapter.slug}.md",
                idea_slug=part.slug,
                part_title=part.title,
                chapter_number=chapter.number,
                chapter_title=chapter.title,
            )
            groups.append(chapter_group)
            chapter_groups.append(chapter_group)

    return groups, front_matter_groups, idea_groups + chapter_groups


def group_for_page(page: int, groups: list[Group]) -> Group:
    for group in groups:
        if group.start_page <= page <= group.end_page:
            return group
    raise RuntimeError(f"No output group found for page {page}.")


def chapter_photo_folder(group: Group) -> str | None:
    if group.kind == "chapter":
        return group.slug
    return None


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content.rstrip() + "\n", encoding="utf-8")


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def render_group_markdown(
    group: Group,
    page_texts: dict[int, str],
    image_manifest: list[dict[str, object]],
    pdf_path: Path,
) -> str:
    pdf_relative = os.path.relpath(pdf_path, group.markdown_path.parent)
    lines = [
        f"# {group.title}",
        "",
        f"Source PDF: `{pdf_relative}`",
        f"Source pages: {group.start_page}-{group.end_page}",
    ]
    if group.part_title:
        lines.append(f"Idea: {group.part_title}")
    if group.chapter_number is not None and group.chapter_title:
        lines.append(f"Chapter number: {group.chapter_number}")

    related = [item for item in image_manifest if item["group_id"] == group.group_id]
    if related:
        lines.extend(["", "Related photos:"])
        for item in related:
            lines.append(
                f"- `{item['relative_photo_path']}` (page {item['page']}, {item['width']}x{item['height']})"
            )

    for page in group.pages:
        page_text = page_texts.get(page, "")
        if not page_text:
            continue
        lines.extend(["", f"## PDF Page {page:03d}", "", "```text", page_text, "```"])
    return "\n".join(lines)


def build_idea_readme(
    idea_folder: Path,
    title: str,
    overview_group: Group,
    chapter_groups: list[Group],
    image_manifest: list[dict[str, object]],
) -> str:
    lines = [
        f"# {title}",
        "",
        "This folder groups markdown and photos for one major idea area from Apple Human Interface Guidelines.",
        "",
        "Structure:",
        "- `markdown/` contains the overview and chapter markdown files.",
        "- `photos/` contains chapter photo folders with extracted PDF images.",
    ]

    overview_images = [item for item in image_manifest if item["group_id"] == overview_group.group_id]
    lines.extend(
        [
            "",
            "Files:",
            f"- `markdown/{overview_group.markdown_path.name}` - idea overview for pages {overview_group.start_page}-{overview_group.end_page}.",
        ]
    )
    if overview_images:
        lines.append(f"- `photos/00-overview/` - {len(overview_images)} photos for the overview pages.")

    for group in chapter_groups:
        count = sum(1 for item in image_manifest if item["group_id"] == group.group_id)
        photo_note = f"; {count} photos in `photos/{group.slug}/`" if count else ""
        lines.append(
            f"- `markdown/{group.markdown_path.name}` - pages {group.start_page}-{group.end_page}{photo_note}."
        )
    return "\n".join(lines)


def build_root_readme(
    output_root: Path,
    page_total: int,
    image_manifest: list[dict[str, object]],
    front_matter_groups: list[Group],
    parts: list[Part],
    chapters: list[Chapter],
) -> str:
    lines = [
        "# Apple Human Interface Guidelines Export",
        "",
        "This folder converts `OSXGuidelines.pdf` into AI-friendly markdown and organizes photos by idea.",
        "",
        "Top-level folders:",
        "- `front_matter/` holds the cover, legal pages, and contents.",
        "- `ideas/` holds one folder per major idea area from the guide.",
        "- `export_manifest.json` describes the full structure.",
        "",
        f"Counts: {page_total} PDF pages, {len(parts) + 1} idea folders, {len(chapters)} chapter markdown files, {len(image_manifest)} extracted photos.",
        "",
        "Idea folders:",
        "- `ideas/00-introduction/` - introduction pages before Part I.",
    ]

    for part in parts:
        part_chapter_count = sum(1 for chapter in chapters if chapter.part_slug == part.slug)
        part_photo_count = sum(1 for item in image_manifest if item["idea_slug"] == part.slug)
        lines.append(
            f"- `ideas/{part.slug}/` - {part.title}; {part_chapter_count} chapter files; {part_photo_count} photos."
        )

    lines.extend(["", "Front matter files:"])
    for group in front_matter_groups:
        lines.append(
            f"- `front_matter/markdown/{group.markdown_path.name}` - pages {group.start_page}-{group.end_page}."
        )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    ensure_tools()

    pdf_path = args.pdf.resolve()
    output_root = args.output.resolve()
    if not pdf_path.exists():
        raise SystemExit(f"Source PDF not found: {pdf_path}")

    reset_output_dir(output_root)

    total_pages = page_count(pdf_path)
    intro_start, parts, chapter_rows = parse_outline(pdf_path)
    chapters = build_chapters(parts, chapter_rows, total_pages)
    part_starts = {part.start_page for part in parts}
    chapter_starts = {chapter.start_page for chapter in chapters}

    page_texts = collect_page_texts(pdf_path, total_pages, intro_start, part_starts, chapter_starts)
    groups, front_matter_groups, idea_and_chapter_groups = build_groups(
        output_root, intro_start, parts, chapters
    )

    raw_image_manifest = extract_raw_images(pdf_path, output_root / "_raw_images")

    full_manifest: list[dict[str, object]] = []
    for item in raw_image_manifest:
        page = int(item["page"])
        group = group_for_page(page, groups)
        if group.idea_slug:
            photo_dir = group.folder / "photos"
            if group.kind == "chapter":
                photo_dir = photo_dir / group.slug
            else:
                photo_dir = photo_dir / "00-overview"
        else:
            photo_dir = group.folder / "photos" / group.slug
        photo_dir.mkdir(parents=True, exist_ok=True)

        source = Path(str(item["temp_path"]))
        destination = photo_dir / source.name
        shutil.move(str(source), destination)

        relative_photo_path = os.path.relpath(destination, group.markdown_path.parent)
        root_relative_path = destination.relative_to(output_root).as_posix()

        full_manifest.append(
            {
                "page": page,
                "page_index": int(item["page_index"]),
                "object_num": int(item["object_num"]),
                "width": int(item["width"]),
                "height": int(item["height"]),
                "group_id": group.group_id,
                "group_slug": group.slug,
                "group_title": group.title,
                "idea_slug": group.idea_slug or "front_matter",
                "photo_path": root_relative_path,
                "relative_photo_path": relative_photo_path,
            }
        )

    raw_dir = output_root / "_raw_images"
    if raw_dir.exists():
        shutil.rmtree(raw_dir)

    for group in groups:
        markdown = render_group_markdown(group, page_texts, full_manifest, pdf_path)
        write_text(group.markdown_path, markdown)

    intro_group = next(group for group in groups if group.idea_slug == "00-introduction")
    intro_readme = build_idea_readme(
        intro_group.folder,
        "Introduction",
        intro_group,
        [],
        full_manifest,
    )
    write_text(intro_group.folder / "README.md", intro_readme)

    for part in parts:
        folder = output_root / "ideas" / part.slug
        overview_group = next(
            group for group in groups if group.idea_slug == part.slug and group.kind == "idea_overview"
        )
        chapter_groups = [
            group
            for group in groups
            if group.idea_slug == part.slug and group.kind == "chapter"
        ]
        write_text(
            folder / "README.md",
            build_idea_readme(folder, part.title, overview_group, chapter_groups, full_manifest),
        )

    front_readme = "\n".join(
        [
            "# Front Matter",
            "",
            "This folder contains the cover, legal pages, and contents extracted from `OSXGuidelines.pdf`.",
            "",
            "Files:",
            "- `markdown/00-cover-and-legal.md`",
            "- `markdown/01-contents.md`",
            "- `photos/` holds any extracted images from those pages.",
        ]
    )
    write_text(output_root / "front_matter" / "README.md", front_readme)

    manifest = {
        "source_pdf": "OSXGuidelines.pdf",
        "page_count": total_pages,
        "intro_start_page": intro_start,
        "parts": [
            {
                "roman": part.roman,
                "title": part.title,
                "start_page": part.start_page,
                "slug": part.slug,
            }
            for part in parts
        ],
        "chapters": [
            {
                "number": chapter.number,
                "title": chapter.title,
                "start_page": chapter.start_page,
                "end_page": chapter.end_page,
                "slug": chapter.slug,
                "idea_slug": chapter.part_slug,
            }
            for chapter in chapters
        ],
        "images": [
            {
                key: value
                for key, value in row.items()
                if key != "relative_photo_path"
            }
            for row in full_manifest
        ],
    }
    write_text(output_root / "export_manifest.json", json.dumps(manifest, indent=2))
    write_csv(output_root / "image_manifest.csv", [
        {
            key: value
            for key, value in row.items()
            if key != "relative_photo_path"
        }
        for row in full_manifest
    ])
    write_text(
        output_root / "README.md",
        build_root_readme(output_root, total_pages, full_manifest, front_matter_groups, parts, chapters),
    )

    print(f"Created organized export at: {output_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
