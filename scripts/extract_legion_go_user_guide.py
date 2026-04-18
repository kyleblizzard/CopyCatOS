#!/usr/bin/env python3
"""Create AI-friendly Markdown exports from the Lenovo Legion Go S user guide."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import unicodedata
from pathlib import Path

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PDF = PROJECT_ROOT / "snowleopardaura" / "lenovo_legion_go_s_ug_en.pdf"
DEFAULT_FULL_OUT = PROJECT_ROOT / "snowleopardaura" / "lenovo_legion_go_guide_markdown"
DEFAULT_HARDWARE_OUT = PROJECT_ROOT / "snowleopardaura" / "lenovo_legion_go_hardware_controls"

SECTION_HEADINGS = [
    "About this guide",
    "Front view",
    "Controls seen from the front view",
    "Joystick light",
    "Speakers",
    "Antennas",
    "Screen",
    "Microphones",
    "Ambient light sensor",
    "Touchpad",
    "Back view",
    "LT and RT button range switch",
    "Controls seen from the back view",
    "Air vents (intake)",
    "Top view",
    "Controls seen from the top view",
    "Air vents (outlet)",
    "Multi-purpose USB Type-C connector",
    "Charging light",
    "Combo audio jack",
    "Volume buttons",
    "Power button",
    "Power light",
    "Bottom view",
    "microSD card slot",
    "Specifications",
    "Statement on USB transfer rate",
    "Using a Power Delivery compliant USB Type-C charger with the console",
    "Avoid constant body contact with specific hot sections",
    "Operating environment",
    "The Legion Space app",
    "Thermal mode",
    "Full speed fan",
    "OS power mode",
    "Adjustable screen resolution",
    "Variable refresh rate",
    "Controller vibration",
    "Connect to an external display",
    "Connect to a wired display",
    "Connect to a wireless display",
    "Change display settings",
    "Set the display mode",
    "Preset shortcuts",
    "Turbo key",
    "Touch gestures",
    "One-finger touch gestures",
    "Two-finger touch gestures",
    "Three- and four-finger touch gestures",
    "Enable three- and four-finger touch gestures",
    "Turn on night light",
    "Adjust color temperature",
    "Rechargeable battery pack",
    "Rapid charge mode",
    "Recover full battery capacity",
    "A power plan",
    "Change or customize a power plan",
    "Open the firmware setup utility",
    "Update the firmware setup utility",
    "Set passwords in UEFI/BIOS setup utility",
    "Password types",
    "Set administrator password",
    "Change or remove administrator password",
    "Set user password",
    "Enable power-on password",
    "Set password for the secondary storage device",
    "Change or remove hard disk password",
    "Frequently asked questions",
    "What should I do if the controller malfunctions?",
    "How to update the drivers and BIOS?",
    "How to change the screen orientation?",
    "How to adjust the dead zone of the joystick?",
    "How to set the gyro?",
    "Self-help resources",
    "What is a CRU",
    "CRUs for your product model",
    "Call Lenovo",
    "Before you contact Lenovo",
    "Purchase additional services",
    "Console and accessibility",
]

HARDWARE_SECTION_ORDER = [
    "Front view",
    "Controls seen from the front view",
    "Joystick light",
    "Screen",
    "Touchpad",
    "Back view",
    "LT and RT button range switch",
    "Controls seen from the back view",
    "Top view",
    "Controls seen from the top view",
    "Multi-purpose USB Type-C connector",
    "Charging light",
    "Combo audio jack",
    "Volume buttons",
    "Power button",
    "Power light",
    "Bottom view",
    "microSD card slot",
    "The Legion Space app",
    "Controller vibration",
    "Preset shortcuts",
    "Turbo key",
    "Touch gestures",
    "One-finger touch gestures",
    "Two-finger touch gestures",
    "Three- and four-finger touch gestures",
    "Enable three- and four-finger touch gestures",
    "What should I do if the controller malfunctions?",
    "How to adjust the dead zone of the joystick?",
    "How to set the gyro?",
]

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
    "\u27a1": "->",
    "\u2799": "->",
    "\u2122": "",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export the Lenovo Legion Go S user guide into AI-friendly Markdown."
    )
    parser.add_argument("--pdf", type=Path, default=DEFAULT_PDF, help="Source PDF path.")
    parser.add_argument(
        "--full-out",
        type=Path,
        default=DEFAULT_FULL_OUT,
        help="Output directory for the full guide export.",
    )
    parser.add_argument(
        "--hardware-out",
        type=Path,
        default=DEFAULT_HARDWARE_OUT,
        help="Output directory for the hardware-controls subset.",
    )
    return parser.parse_args()


def ensure_tools() -> None:
    required = ["pdfinfo", "pdftotext", "pdfimages", "pdftoppm"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        joined = ", ".join(missing)
        raise SystemExit(f"Missing required tools: {joined}")


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


def strip_edge_blanks(lines: list[str]) -> list[str]:
    start = 0
    end = len(lines)
    while start < end and not lines[start].strip():
        start += 1
    while end > start and not lines[end - 1].strip():
        end -= 1
    return lines[start:end]


def cleanup_page_text(text: str, page_number: int) -> str:
    lines = [line.rstrip() for line in normalize_ascii(text).splitlines()]
    lines = strip_edge_blanks(lines)

    nonempty_indices = [index for index, line in enumerate(lines) if line.strip()]
    order_lookup = {index: position for position, index in enumerate(nonempty_indices)}
    last_nonempty = len(nonempty_indices) - 1

    cleaned: list[str] = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped:
            if cleaned and cleaned[-1] != "":
                cleaned.append("")
            continue

        order = order_lookup.get(index, 0)
        in_margin = order <= 2 or order >= last_nonempty - 2

        if stripped == "User Guide":
            continue
        if stripped.startswith("(c) Copyright Lenovo"):
            continue
        if in_margin and re.fullmatch(r"[ivxlcdm]+", stripped.lower()):
            continue
        if in_margin and re.fullmatch(r"\d+", stripped):
            continue
        if in_margin and re.fullmatch(r"\d+\s+User Guide", stripped):
            continue
        if in_margin and re.fullmatch(r"Chapter \d+\..*\s+\d+", stripped):
            continue
        if in_margin and stripped == str(page_number):
            continue

        cleaned.append(line)

    cleaned = strip_edge_blanks(cleaned)
    return "\n".join(cleaned).strip()


def parse_page_count(pdf_path: Path) -> int:
    info = run_text("pdfinfo", str(pdf_path))
    match = re.search(r"^Pages:\s+(\d+)$", info, re.MULTILINE)
    if not match:
        raise RuntimeError("Unable to determine page count from pdfinfo output.")
    return int(match.group(1))


def collect_page_texts(pdf_path: Path, page_count: int) -> dict[int, str]:
    pages: dict[int, str] = {}
    for page in range(1, page_count + 1):
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
        pages[page] = cleanup_page_text(raw, page)
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


def extract_images(pdf_path: Path, images_dir: Path) -> list[dict[str, object]]:
    rows = parse_pdfimages_rows(pdf_path)
    temp_dir = images_dir / "_raw"
    temp_dir.mkdir(parents=True, exist_ok=True)
    prefix = temp_dir / "image"
    run_command("pdfimages", "-png", str(pdf_path), str(prefix))

    raw_files = sorted(temp_dir.glob("image-*.png"))
    if len(raw_files) != len(rows):
        raise RuntimeError(
            f"Expected {len(rows)} extracted images, found {len(raw_files)}."
        )

    images_dir.mkdir(parents=True, exist_ok=True)
    per_page_counter: dict[int, int] = {}
    manifest: list[dict[str, object]] = []

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
        file_name = f"page-{page:03d}-image-{page_index:02d}.png"
        destination = images_dir / file_name

        next_is_mask = (
            index + 1 < len(rows)
            and rows[index + 1]["type"] == "smask"
            and int(rows[index + 1]["page"]) == page
        )

        if next_is_mask:
            with Image.open(raw_path) as base_image, Image.open(raw_files[index + 1]) as mask_image:
                merged = base_image.convert("RGBA")
                mask = mask_image.convert("L")
                if mask.size == merged.size:
                    merged.putalpha(mask)
                merged.save(destination)
            index += 1
        else:
            shutil.copy2(raw_path, destination)

        manifest.append(
            {
                "page": page,
                "page_index": page_index,
                "object_num": int(row["object_num"]),
                "width": int(row["width"]),
                "height": int(row["height"]),
                "path": f"images/{file_name}",
            }
        )
        index += 1

    shutil.rmtree(temp_dir)
    return manifest


def render_visual_pages(pdf_path: Path, pages: set[int], render_dir: Path) -> dict[int, str]:
    render_dir.mkdir(parents=True, exist_ok=True)
    mapping: dict[int, str] = {}
    for page in sorted(pages):
        output_base = render_dir / f"page-{page:03d}"
        run_command(
            "pdftoppm",
            "-f",
            str(page),
            "-l",
            str(page),
            "-png",
            "-singlefile",
            str(pdf_path),
            str(output_base),
        )
        mapping[page] = f"page_renders/page-{page:03d}.png"
    return mapping


def title_for_page(page_text: str) -> str:
    for line in page_text.splitlines():
        if line.strip():
            return line.strip()
    return "Untitled page"


def images_by_page(manifest: list[dict[str, object]]) -> dict[int, list[dict[str, object]]]:
    by_page: dict[int, list[dict[str, object]]] = {}
    for item in manifest:
        by_page.setdefault(int(item["page"]), []).append(item)
    return by_page


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content.rstrip() + "\n", encoding="utf-8")


def write_image_manifest(path: Path, manifest: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["page", "page_index", "object_num", "width", "height", "path"],
        )
        writer.writeheader()
        writer.writerows(manifest)


def build_page_markdown(
    page: int,
    page_text: str,
    page_title: str,
    page_render: str | None,
    images: list[dict[str, object]],
) -> str:
    lines = [
        f"# PDF Page {page:03d}",
        "",
        f"Page title: {page_title}",
        "Source PDF: ../../lenovo_legion_go_s_ug_en.pdf",
    ]
    if page_render:
        lines.extend(["", f"Page render: `{page_render}`", "", f"![PDF page {page:03d} render](../{page_render})"])
    if images:
        lines.append("")
        lines.append("Extracted images:")
        for image in images:
            lines.append(
                f"- `../{image['path']}` ({image['width']}x{image['height']}, object {image['object_num']})"
            )
    lines.extend(["", "```text", page_text, "```"])
    return "\n".join(lines)


def build_full_guide_markdown(
    page_count: int,
    page_titles: dict[int, str],
    page_texts: dict[int, str],
    page_render_map: dict[int, str],
    page_image_map: dict[int, list[dict[str, object]]],
) -> str:
    lines = [
        "# Lenovo Legion Go S User Guide",
        "",
        "Source PDF: `../lenovo_legion_go_s_ug_en.pdf`",
        f"Total PDF pages: {page_count}",
        "Notes: text was normalized to ASCII and obvious headers/footers were removed.",
        "Companion files: `pages/`, `images/`, `page_renders/`, `page_manifest.json`, `image_manifest.csv`.",
    ]

    for page in range(1, page_count + 1):
        lines.extend(
            [
                "",
                f"## PDF Page {page:03d}",
                "",
                f"Page file: `pages/page-{page:03d}.md`",
                f"Page title: {page_titles[page]}",
            ]
        )
        if page in page_render_map:
            lines.append(f"Page render: `{page_render_map[page]}`")
        if page in page_image_map:
            lines.append("Extracted images:")
            for image in page_image_map[page]:
                lines.append(
                    f"- `{image['path']}` ({image['width']}x{image['height']}, object {image['object_num']})"
                )
        lines.extend(["", "```text", page_texts[page], "```"])

    return "\n".join(lines)


def build_full_readme(page_count: int, image_count: int, render_count: int) -> str:
    lines = [
        "# Lenovo Legion Go S Guide Export",
        "",
        "This folder contains an AI-friendly export of `../lenovo_legion_go_s_ug_en.pdf`.",
        "",
        "Files:",
        "- `guide.md` - combined Markdown export, organized by PDF page.",
        "- `pages/` - one Markdown file per PDF page.",
        "- `images/` - embedded images extracted from the PDF.",
        "- `page_renders/` - PNG renders for visual pages.",
        "- `page_manifest.json` - per-page metadata and visual mappings.",
        "- `image_manifest.csv` - flat image inventory.",
        "",
        f"Counts: {page_count} pages, {image_count} extracted images, {render_count} rendered pages.",
    ]
    return "\n".join(lines)


def extract_sections(page_texts: dict[int, str]) -> dict[str, dict[str, object]]:
    heading_set = set(SECTION_HEADINGS)
    sections: dict[str, dict[str, object]] = {}
    current_heading: str | None = None
    current_lines: list[str] = []
    current_pages: set[int] = set()

    def flush() -> None:
        nonlocal current_heading, current_lines, current_pages
        if current_heading is None:
            return
        lines = strip_edge_blanks(current_lines)
        if current_heading not in sections:
            sections[current_heading] = {
                "heading": current_heading,
                "start_page": min(current_pages) if current_pages else None,
                "pages": sorted(current_pages),
                "content": "\n".join(lines).strip(),
            }
        current_heading = None
        current_lines = []
        current_pages = set()

    for page in sorted(page_texts):
        for line in page_texts[page].splitlines():
            stripped = line.strip()
            if re.fullmatch(r"Chapter \d+\..*", stripped):
                flush()
                current_heading = stripped
                current_lines = []
                current_pages = {page}
                continue
            if stripped in heading_set:
                flush()
                current_heading = stripped
                current_lines = []
                current_pages = {page}
                continue
            if current_heading is None:
                continue
            current_lines.append(line)
            current_pages.add(page)

    flush()
    return sections


def section_visual_page(section: dict[str, object], render_map: dict[int, str]) -> int | None:
    for page in section["pages"]:
        if page in render_map:
            return page
    return None


def build_hardware_markdown(
    sections: dict[str, dict[str, object]],
    render_map: dict[int, str],
    page_image_map: dict[int, list[dict[str, object]]],
) -> tuple[str, dict[str, object]]:
    lines = [
        "# Lenovo Legion Go S Hardware Controls",
        "",
        "Source PDF: `../lenovo_legion_go_s_ug_en.pdf`",
        "This subset keeps the sections that describe physical controls, controller behavior, touch input, and related controller troubleshooting.",
    ]

    manifest_sections: list[dict[str, object]] = []

    for heading in HARDWARE_SECTION_ORDER:
        section = sections.get(heading)
        if not section or not section["content"]:
            continue

        visual_page = section_visual_page(section, render_map)
        lines.extend(["", f"## {heading}", ""])
        lines.append(f"Source page(s): {', '.join(str(page) for page in section['pages'])}")
        if visual_page is not None:
            render_path = render_map[visual_page]
            lines.extend(["", f"Visual reference: `{render_path}`", "", f"![{heading}]({render_path})"])
        for page in section["pages"]:
            if page in page_image_map:
                lines.append(
                    "Extracted images: "
                    + ", ".join(f"`{image['path']}`" for image in page_image_map[page])
                )
        lines.extend(["", "```text", str(section["content"]), "```"])

        manifest_sections.append(
            {
                "heading": heading,
                "start_page": section["start_page"],
                "pages": section["pages"],
                "page_render": render_map.get(visual_page) if visual_page is not None else None,
                "images": [
                    image["path"]
                    for page in section["pages"]
                    for image in page_image_map.get(page, [])
                ],
            }
        )

    metadata = {
        "source_pdf": "../lenovo_legion_go_s_ug_en.pdf",
        "sections": manifest_sections,
    }
    return "\n".join(lines), metadata


def build_hardware_readme(section_count: int, image_count: int, render_count: int) -> str:
    lines = [
        "# Lenovo Legion Go S Hardware Controls Export",
        "",
        "This folder contains only the guide content related to physical controls, controller behavior, touch input, and controller troubleshooting.",
        "",
        "Files:",
        "- `hardware-controls.md` - curated subset of the guide.",
        "- `images/` - extracted images for the included sections.",
        "- `page_renders/` - rendered reference pages for illustrated control sections.",
        "- `hardware_manifest.json` - section-level metadata.",
        "",
        f"Counts: {section_count} sections, {image_count} extracted images, {render_count} rendered pages.",
    ]
    return "\n".join(lines)


def copy_subset_assets(
    manifest: list[dict[str, object]],
    render_map: dict[int, str],
    used_pages: set[int],
    full_out: Path,
    hardware_out: Path,
) -> tuple[dict[int, str], list[dict[str, object]]]:
    hardware_images_dir = hardware_out / "images"
    hardware_renders_dir = hardware_out / "page_renders"
    hardware_images_dir.mkdir(parents=True, exist_ok=True)
    hardware_renders_dir.mkdir(parents=True, exist_ok=True)

    subset_manifest: list[dict[str, object]] = []
    for item in manifest:
        if int(item["page"]) not in used_pages:
            continue
        source = full_out / str(item["path"])
        destination = hardware_images_dir / source.name
        shutil.copy2(source, destination)
        subset_manifest.append(
            {
                **item,
                "path": f"images/{destination.name}",
            }
        )

    subset_render_map: dict[int, str] = {}
    for page, render_path in render_map.items():
        if page not in used_pages:
            continue
        source = full_out / render_path
        destination = hardware_renders_dir / source.name
        shutil.copy2(source, destination)
        subset_render_map[page] = f"page_renders/{destination.name}"

    return subset_render_map, subset_manifest


def reset_output_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def main() -> int:
    args = parse_args()
    ensure_tools()

    pdf_path = args.pdf.resolve()
    full_out = args.full_out.resolve()
    hardware_out = args.hardware_out.resolve()

    if not pdf_path.exists():
        raise SystemExit(f"Source PDF not found: {pdf_path}")

    reset_output_dir(full_out)
    reset_output_dir(hardware_out)

    page_count = parse_page_count(pdf_path)
    page_texts = collect_page_texts(pdf_path, page_count)
    page_titles = {page: title_for_page(text) for page, text in page_texts.items()}

    image_manifest = extract_images(pdf_path, full_out / "images")
    page_image_map = images_by_page(image_manifest)
    render_map = render_visual_pages(pdf_path, set(page_image_map), full_out / "page_renders")

    page_manifest: list[dict[str, object]] = []
    for page in range(1, page_count + 1):
        page_file = full_out / "pages" / f"page-{page:03d}.md"
        page_doc = build_page_markdown(
            page,
            page_texts[page],
            page_titles[page],
            render_map.get(page),
            page_image_map.get(page, []),
        )
        write_text(page_file, page_doc)
        page_manifest.append(
            {
                "page": page,
                "title": page_titles[page],
                "page_file": f"pages/page-{page:03d}.md",
                "page_render": render_map.get(page),
                "images": [item["path"] for item in page_image_map.get(page, [])],
            }
        )

    full_guide = build_full_guide_markdown(
        page_count,
        page_titles,
        page_texts,
        render_map,
        page_image_map,
    )
    write_text(full_out / "guide.md", full_guide)
    write_text(
        full_out / "README.md",
        build_full_readme(page_count, len(image_manifest), len(render_map)),
    )
    write_image_manifest(full_out / "image_manifest.csv", image_manifest)
    write_text(full_out / "page_manifest.json", json.dumps(page_manifest, indent=2))

    sections = extract_sections(page_texts)
    selected_sections = [sections[name] for name in HARDWARE_SECTION_ORDER if name in sections]
    used_pages = {page for section in selected_sections for page in section["pages"]}
    hardware_render_map, hardware_image_manifest = copy_subset_assets(
        image_manifest,
        render_map,
        used_pages,
        full_out,
        hardware_out,
    )
    hardware_page_image_map = images_by_page(hardware_image_manifest)
    hardware_md, hardware_metadata = build_hardware_markdown(
        sections,
        hardware_render_map,
        hardware_page_image_map,
    )
    write_text(hardware_out / "hardware-controls.md", hardware_md)
    write_text(
        hardware_out / "README.md",
        build_hardware_readme(
            len(hardware_metadata["sections"]),
            len(hardware_image_manifest),
            len(hardware_render_map),
        ),
    )
    write_text(
        hardware_out / "hardware_manifest.json",
        json.dumps(hardware_metadata, indent=2),
    )

    print(f"Created full guide export at: {full_out}")
    print(f"Created hardware-controls export at: {hardware_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
