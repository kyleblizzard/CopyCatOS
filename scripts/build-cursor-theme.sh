#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

#
# build-cursor-theme.sh
# ---------------------
# Builds a complete X11 cursor theme from extracted Snow Leopard cursor PNGs.
# Splits animated sprite sheets into individual frames, scales static cursors
# to the HiDPI target sizes (multi-source: prefers vector-rasterized @2x/@4x
# PSD sources, falls back to nearest-neighbor upscale of the 1x pixel-art
# source when @Nx isn't available), generates xcursorgen config files, and
# creates all freedesktop symlinks.
#
# Multi-source HiDPI pipeline:
#   - snowleopardaura/MacAssets/Cursors/<name>.png     (1x source, always)
#   - snowleopardaura/MacAssets/Cursors/@2x/<name>.png (optional, PSD-derived)
#   - snowleopardaura/MacAssets/Cursors/@4x/<name>.png (optional, PSD-derived)
# At matching target factors the @Nx dimensions equal the nearest-neighbor
# output, so @Nx substitution is a pure pixel-quality upgrade (no geometry
# or hotspot drift). Animated cursors stay on the 1x path — the PSD ships
# single frames only.
#
# Usage: bash scripts/build-cursor-theme.sh
# Run from the CopyCatOS repo root.

set -e

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Where the extracted cursor PNGs live
SRC_DIR="snowleopardaura/MacAssets/Cursors"

# Where the finished X11 cursor theme goes
OUT_DIR="cursors/SnowLeopard"

# Temporary working directory for intermediate files (frames, scaled PNGs, configs)
WORK_DIR="/tmp/snowleopard-cursors-build"

# Target sizes for multi-resolution cursors.
# 32 is the primary size; 48 provides a HiDPI option.
TARGET_SIZES=(32 48)

# ---------------------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------------------

echo "=== Snow Leopard X11 Cursor Theme Builder ==="
echo ""

# We need xcursorgen to compile .cursor config files into X11 cursor format
if ! command -v xcursorgen &>/dev/null; then
    echo "ERROR: xcursorgen not found."
    echo "  Fedora/Nobara: sudo dnf install xcursorgen"
    echo "  Debian/Ubuntu: sudo apt install x11-apps"
    echo "  macOS (Homebrew): brew install xcursorgen"
    exit 1
fi

# We need Python 3 with PIL/Pillow for image splitting and scaling
if ! python3 -c "from PIL import Image" &>/dev/null; then
    echo "ERROR: Python 3 with Pillow (PIL) not found."
    echo "  Install with: pip3 install Pillow"
    exit 1
fi

echo "[OK] xcursorgen found"
echo "[OK] Python 3 + Pillow found"
echo ""

# ---------------------------------------------------------------------------
# Directory setup
# ---------------------------------------------------------------------------

# Clean any previous build artifacts
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/frames"    # Individual frames extracted from sprite sheets
mkdir -p "$WORK_DIR/scaled"    # Upscaled PNGs at each target size
mkdir -p "$WORK_DIR/configs"   # xcursorgen .cursor config files

# Create the output theme directory structure
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/cursors"

echo "Source:  $SRC_DIR"
echo "Output:  $OUT_DIR"
echo "Work:    $WORK_DIR"
echo ""

# ---------------------------------------------------------------------------
# Step 1: Split animated sprite sheets into individual frame PNGs
# ---------------------------------------------------------------------------
# Animated cursors are stored as vertical sprite sheets — all frames stacked
# top-to-bottom in a single PNG. We split them into separate frame files
# so xcursorgen can assemble them with per-frame timing.

echo "--- Step 1: Splitting animated sprite sheets ---"

python3 << 'PYEOF'
from PIL import Image
import os, sys

src = "snowleopardaura/MacAssets/Cursors"
frames_dir = "/tmp/snowleopard-cursors-build/frames"

# Each entry: (filename, frame_width, frame_height, num_frames)
# The sprite sheet is frame_width x (frame_height * num_frames)
animations = [
    ("spinning_wait.png", 16, 16, 10),
    ("alias_arrow.png",   50, 50, 15),
    ("counting_down.png", 16, 16, 6),
    ("counting_up_down.png", 16, 16, 6),
]

for filename, fw, fh, nframes in animations:
    path = os.path.join(src, filename)
    if not os.path.exists(path):
        print(f"  WARNING: {filename} not found, skipping")
        continue

    img = Image.open(path)
    base = filename.replace(".png", "")

    # Verify dimensions match expectations
    expected_h = fh * nframes
    if img.size != (fw, expected_h):
        print(f"  WARNING: {filename} is {img.size[0]}x{img.size[1]}, "
              f"expected {fw}x{expected_h}")

    # Crop each frame from top to bottom
    for i in range(nframes):
        # Each frame starts at y = i * frame_height
        box = (0, i * fh, fw, (i + 1) * fh)
        frame = img.crop(box)
        out_path = os.path.join(frames_dir, f"{base}_{i:02d}.png")
        frame.save(out_path)

    print(f"  {filename} -> {nframes} frames ({fw}x{fh} each)")

print("  Done.")
PYEOF

echo ""

# ---------------------------------------------------------------------------
# Step 2: Upscale all cursors using nearest-neighbor interpolation
# ---------------------------------------------------------------------------
# Most Snow Leopard cursors are 16x16, which is too small for modern displays.
# We scale everything up to 32x32 and 48x48 using nearest-neighbor to preserve
# the crisp pixel art without any blurring. Non-square images are centered on
# a transparent canvas to maintain the aspect ratio.
#
# For cursors already at or above the target size (e.g. 32x32, 64x64),
# we scale to fit within the target and center on a transparent canvas.

echo "--- Step 2: Upscaling cursors to ${TARGET_SIZES[*]} ---"

python3 << 'PYEOF'
from PIL import Image
import os, sys, math

src = "snowleopardaura/MacAssets/Cursors"
frames_dir = "/tmp/snowleopard-cursors-build/frames"
scaled_dir = "/tmp/snowleopard-cursors-build/scaled"
target_sizes = [32, 48]

# ---- Static cursors ----
# We list them explicitly so we know exactly which files to process.
static_cursors = [
    "arrow.png",
    "contextual_menu.png",
    "copy_arrow.png",
    "ibeam.png",
    "crosshair_0.png",
    "plus_0.png",
    "pointing_hand.png",
    "open_hand.png",
    "closed_hand_0.png",
    "not_allowed.png",
    "poof.png",
    "drag_link.png",
    "resize_left.png",
    "resize_right.png",
    "resize_left_right.png",
    "resize_up.png",
    "resize_down.png",
    "resize_up_down.png",
    "watch_0.png",
    "counting_up.png",
    "zoom_in.png",
    "zoom_out.png",
    "fleur.png",
]

# ---- Animated cursor frames (already split in Step 1) ----
anim_frames = []
for base, count in [("spinning_wait", 10), ("alias_arrow", 15),
                     ("counting_down", 6), ("counting_up_down", 6)]:
    for i in range(count):
        anim_frames.append(f"{base}_{i:02d}.png")


def scale_cursor(img, nominal_size):
    """
    Scale a cursor image for a given nominal size.

    Nearest-neighbor for pixel-art (1×) sources — preserves the crisp
    hand-drawn look Snow Leopard cursors rely on. For multi-source
    HiDPI path, see pick_hires_source().

    Returns (scaled_image, scale_factor).
    """
    w, h = img.size
    min_dim = min(w, h)
    max_dim = max(w, h)

    if min_dim < nominal_size:
        scale = max(1, nominal_size // min_dim)
        new_w = w * scale
        new_h = h * scale
        scaled = img.resize((new_w, new_h), Image.NEAREST)
        return scaled, scale
    elif min_dim == nominal_size and max_dim == nominal_size:
        return img.copy(), 1
    else:
        scale_f = nominal_size / max_dim
        new_w = max(1, int(w * scale_f))
        new_h = max(1, int(h * scale_f))
        scaled = img.resize((new_w, new_h), Image.NEAREST)
        return scaled, scale_f


def pick_hires_source(cursor_base, factor, target_w, target_h):
    """
    For a static cursor, try to substitute a vector-rasterized @Nx PSD source
    in place of the nearest-neighbor upscale of the 1× pixel-art PNG.

    The @2× and @4× PSDs are genuine re-rasterizations — anti-aliased edges
    and refined sub-pixel detail — not upscales. At matching factors the
    @Nx image dimensions equal `target_w × target_h` (the natural output of
    the upscale path), so the swap is pixel-dimension-identical.

    Returns a PIL Image at (target_w, target_h), or None if no @Nx source
    exists for this cursor. factor must be 1, 2, or 4; for factor=3 we
    downscale @4× with Lanczos.
    """
    # factor 1 means no upscale needed — caller handles 1× directly.
    if factor < 2:
        return None

    # Try the matching @Nx tier first, then fall back to @4× downscale.
    candidates = [factor] if factor in (2, 4) else [4, 2]
    if factor == 3:
        candidates = [4, 2]

    for try_factor in candidates:
        path = os.path.join(src, f"@{try_factor}x", f"{cursor_base}.png")
        if not os.path.exists(path):
            continue
        img = Image.open(path).convert("RGBA")
        if img.size == (target_w, target_h):
            return img
        # Lanczos resample — the @Nx sources are anti-aliased so bilinear/
        # Lanczos is appropriate. Nearest would reintroduce pixel-art blockiness.
        return img.resize((target_w, target_h), Image.LANCZOS)

    return None


def process_file(filepath, filename, nominal_size, hires=False):
    """
    Scale one image file and save to the scaled directory.

    When `hires` is True, try to substitute a @2x/@4x PSD source at the same
    target dimensions. Falls back to nearest-neighbor upscale if no @Nx
    source is available for this cursor.
    """
    img = Image.open(filepath).convert("RGBA")
    scaled, scale = scale_cursor(img, nominal_size)

    base = filename.replace(".png", "")

    if hires and isinstance(scale, int) and scale >= 2:
        # Integer-factor upscale path — try the @Nx vector source instead.
        hi = pick_hires_source(base, scale, scaled.size[0], scaled.size[1])
        if hi is not None:
            scaled = hi

    out_path = os.path.join(scaled_dir, f"{base}_{nominal_size}.png")
    scaled.save(out_path)
    return scale


# Process static cursors — prefer vector-rasterized @Nx PSD sources when
# available. Output dimensions are identical to the legacy nearest-neighbor
# path, so hotspot math (Step 3) remains unchanged.
hires_hits = 0
hires_misses = 0
for filename in static_cursors:
    filepath = os.path.join(src, filename)
    if not os.path.exists(filepath):
        print(f"  WARNING: {filename} not found, skipping")
        continue
    base = filename.replace(".png", "")
    has_hires = any(
        os.path.exists(os.path.join(src, f"@{f}x", filename)) for f in (2, 4)
    )
    if has_hires:
        hires_hits += 1
    else:
        hires_misses += 1
    for ts in target_sizes:
        scale = process_file(filepath, filename, ts, hires=True)
    img = Image.open(filepath)
    tag = "hires" if has_hires else "1x-only"
    print(f"  {filename} ({img.size[0]}x{img.size[1]}) [{tag}] -> scaled to {target_sizes}")

print(f"  Hires source hits: {hires_hits}, 1x-only fallback: {hires_misses}")

# Process animated frames — PSD ships single frames only, so animated
# sources stay on the 1× pixel-art nearest-neighbor path.
for filename in anim_frames:
    filepath = os.path.join(frames_dir, filename)
    if not os.path.exists(filepath):
        print(f"  WARNING: frame {filename} not found, skipping")
        continue
    for ts in target_sizes:
        process_file(filepath, filename, ts, hires=False)

print(f"  Animated frames scaled: {len(anim_frames)} frames")
print("  Done.")
PYEOF

echo ""

# ---------------------------------------------------------------------------
# Step 3: Compute scaled hotspots and generate xcursorgen config files
# ---------------------------------------------------------------------------
# xcursorgen needs a config file for each cursor that specifies:
#   <size> <xhot> <yhot> <filename> [delay_ms]
# We compute the hotspot positions after scaling/padding. For multi-size
# cursors, each size gets its own line.

echo "--- Step 3: Generating xcursorgen config files ---"

python3 << 'PYEOF'
from PIL import Image
import os, math

src = "snowleopardaura/MacAssets/Cursors"
frames_dir = "/tmp/snowleopard-cursors-build/frames"
scaled_dir = "/tmp/snowleopard-cursors-build/scaled"
configs_dir = "/tmp/snowleopard-cursors-build/configs"
target_sizes = [32, 48]


def compute_scaled_hotspot(orig_w, orig_h, hot_x, hot_y, nominal_size):
    """
    Given the original image dimensions and hotspot, compute where the
    hotspot lands after scaling. Must match the scale_cursor() logic
    from Step 2 exactly (no padding, just scale).
    Returns (new_hot_x, new_hot_y).
    """
    min_dim = min(orig_w, orig_h)
    max_dim = max(orig_w, orig_h)

    if min_dim < nominal_size:
        # Integer scale up based on shorter side — same as Step 2
        scale = max(1, nominal_size // min_dim)
        return hot_x * scale, hot_y * scale
    elif min_dim == nominal_size and max_dim == nominal_size:
        # Already exact size
        return hot_x, hot_y
    else:
        # Float scale down — same as Step 2
        scale_f = nominal_size / max_dim
        return int(hot_x * scale_f), int(hot_y * scale_f)


# ---- Static cursor definitions ----
# (cursor_name, source_filename, orig_w, orig_h, hot_x, hot_y)
static_defs = [
    ("left_ptr",            "arrow",            12, 19, 0,  0),
    ("context_menu",        "contextual_menu",  17, 19, 0,  0),
    ("copy",                "copy_arrow",       19, 32, 0,  0),
    ("xterm",               "ibeam",             7, 17, 3,  8),
    ("crosshair",           "crosshair_0",      23, 23, 11, 11),
    ("cell",                "plus_0",           14, 14, 7,  7),
    ("hand2",               "pointing_hand",    16, 17, 4,  0),
    ("openhand",            "open_hand",        15, 14, 7,  7),
    ("closedhand",          "closed_hand_0",    16, 16, 8,  8),
    ("not-allowed",         "not_allowed",      16, 16, 8,  8),
    ("pirate",              "poof",             16, 16, 8,  8),
    ("dnd-link",            "drag_link",        19, 32, 0,  0),
    ("left_side",           "resize_left",      13, 13, 6,  6),
    ("right_side",          "resize_right",     13, 12, 6,  6),
    ("sb_h_double_arrow",   "resize_left_right",16, 13, 8,  6),
    ("top_side",            "resize_up",        12, 13, 6,  6),
    ("bottom_side",         "resize_down",      12, 13, 6,  6),
    ("sb_v_double_arrow",   "resize_up_down",   12, 16, 6,  8),
    ("watch",               "watch_0",          64, 64, 32, 32),
    ("counting_up_cursor",  "counting_up",      16, 16, 8,  8),
    ("zoom-in",             "zoom_in",          16, 16, 6,  6),
    ("zoom-out",            "zoom_out",         16, 16, 6,  6),
    ("fleur",               "fleur",            16, 16, 8,  8),
]

# ---- Animated cursor definitions ----
# (cursor_name, frame_base, frame_w, frame_h, hot_x, hot_y, num_frames, delay_ms)
anim_defs = [
    ("wait",                "spinning_wait",    16, 16, 8,  8,  10, 100),
    ("dnd-link-anim",       "alias_arrow",      50, 50, 25, 25, 15, 66),
    ("counting_down",       "counting_down",    16, 16, 8,  8,  6,  100),
    ("counting_up_down",    "counting_up_down", 16, 16, 8,  8,  6,  100),
]

# Generate config files for static cursors
for cursor_name, src_base, ow, oh, hx, hy in static_defs:
    config_path = os.path.join(configs_dir, f"{cursor_name}.cursor")
    with open(config_path, "w") as f:
        for ts in target_sizes:
            shx, shy = compute_scaled_hotspot(ow, oh, hx, hy, ts)
            png_path = os.path.join(scaled_dir, f"{src_base}_{ts}.png")
            f.write(f"{ts} {shx} {shy} {png_path}\n")
    print(f"  {cursor_name}.cursor (static)")

# Generate config files for animated cursors
for cursor_name, frame_base, fw, fh, hx, hy, nframes, delay in anim_defs:
    config_path = os.path.join(configs_dir, f"{cursor_name}.cursor")
    with open(config_path, "w") as f:
        for i in range(nframes):
            for ts in target_sizes:
                shx, shy = compute_scaled_hotspot(fw, fh, hx, hy, ts)
                png_path = os.path.join(scaled_dir, f"{frame_base}_{i:02d}_{ts}.png")
                f.write(f"{ts} {shx} {shy} {png_path} {delay}\n")
    print(f"  {cursor_name}.cursor (animated, {nframes} frames, {delay}ms)")

print("  Done.")
PYEOF

echo ""

# ---------------------------------------------------------------------------
# Step 4: Run xcursorgen to compile each cursor
# ---------------------------------------------------------------------------
# xcursorgen reads a config file and produces a binary X11 cursor file.
# Each output file goes into OUT_DIR/cursors/<cursor_name>.

echo "--- Step 4: Compiling cursors with xcursorgen ---"

for config in "$WORK_DIR"/configs/*.cursor; do
    # Extract cursor name from filename (e.g., left_ptr.cursor -> left_ptr)
    cursor_name=$(basename "$config" .cursor)
    output="$OUT_DIR/cursors/$cursor_name"

    if xcursorgen "$config" "$output" 2>/dev/null; then
        echo "  [OK] $cursor_name"
    else
        echo "  [FAIL] $cursor_name — xcursorgen failed"
        # Show the config for debugging
        echo "    Config contents:"
        head -5 "$config" | sed 's/^/      /'
    fi
done

echo ""

# ---------------------------------------------------------------------------
# Step 5: Create freedesktop symlinks
# ---------------------------------------------------------------------------
# X11 cursor themes use symlinks to map alternative cursor names to the
# canonical cursor files. For example, "pointer" -> "hand2" means any app
# requesting the "pointer" cursor will get our hand2 cursor.
#
# Some apps use cursor name hashes (the long hex strings) instead of
# human-readable names — those are legacy X cursor font name hashes.

echo "--- Step 5: Creating freedesktop symlinks ---"

cd "$OUT_DIR/cursors"

# Helper: create a symlink if the target cursor file exists
make_link() {
    local target="$1"
    local link_name="$2"

    if [ -f "$target" ]; then
        ln -sf "$target" "$link_name"
        echo "  $link_name -> $target"
    else
        echo "  [SKIP] $link_name -> $target (target missing)"
    fi
}

# --- left_ptr (the default arrow cursor) ---
make_link "left_ptr" "arrow"
make_link "left_ptr" "default"
make_link "left_ptr" "top_left_arrow"

# --- xterm (text selection I-beam) ---
make_link "xterm" "text"
make_link "xterm" "ibeam"

# --- hand2 (clickable link pointer) ---
make_link "hand2" "pointer"
make_link "hand2" "pointing_hand"
make_link "hand2" "hand1"
make_link "hand2" "hand"
# Legacy hash for "pointer" used by some GTK apps
make_link "hand2" "e29285e634086352946a0e7090d73106"

# --- crosshair ---
make_link "crosshair" "cross"
make_link "crosshair" "tcross"

# --- not-allowed (forbidden/no-drop) ---
make_link "not-allowed" "forbidden"
make_link "not-allowed" "no-drop"
make_link "not-allowed" "dnd-no-drop"
make_link "not-allowed" "crossed_circle"
# Legacy hash for "crossed_circle"
make_link "not-allowed" "03b6e0fcb3499374a867d041f52298f0"

# --- sb_h_double_arrow (horizontal resize) ---
make_link "sb_h_double_arrow" "ew-resize"
make_link "sb_h_double_arrow" "col-resize"
make_link "sb_h_double_arrow" "size_hor"
make_link "sb_h_double_arrow" "split_h"
make_link "sb_h_double_arrow" "h_double_arrow"
# Legacy hashes for horizontal resize cursors
make_link "sb_h_double_arrow" "14fef782d02440884392942c11205230"
make_link "sb_h_double_arrow" "028006030e0e7ebffc7f7070c0600140"

# --- sb_v_double_arrow (vertical resize) ---
make_link "sb_v_double_arrow" "ns-resize"
make_link "sb_v_double_arrow" "row-resize"
make_link "sb_v_double_arrow" "size_ver"
make_link "sb_v_double_arrow" "split_v"
make_link "sb_v_double_arrow" "v_double_arrow"
# Legacy hash for vertical resize
make_link "sb_v_double_arrow" "2870a09082c103050810ffdffffe0204"

# --- Directional resize edges ---
make_link "left_side" "w-resize"
make_link "right_side" "e-resize"
make_link "top_side" "n-resize"
make_link "bottom_side" "s-resize"

# --- openhand (grab cursor before clicking) ---
# Only "grab" — the hand-open glyph means "ready to grab something you're about
# to click-drag." It is not a move cursor; move/size_all/all-scroll go to fleur.
make_link "openhand" "grab"

# --- closedhand (grabbing / actively dragging) ---
# Only "grabbing" — the closed-fist glyph means "currently holding." It is not
# a move cursor; dnd-move / move go to fleur.
make_link "closedhand" "grabbing"

# --- fleur (4-way move / scroll-all) ---
# Classic Mac "move" shape. Used whenever the action is "translate this thing
# in any direction" rather than "grab" (open hand) or "holding" (fist).
make_link "fleur" "size_all"
make_link "fleur" "all-scroll"
make_link "fleur" "move"
make_link "fleur" "dnd-move"

# --- zoom-in / zoom-out ---
# Magnifier cursors. Apps using the CSS spec cursor names need both hyphenated
# and underscore spellings, so wire both.
make_link "zoom-in" "zoom_in"
make_link "zoom-out" "zoom_out"

# --- copy (drag-copy cursor) ---
make_link "copy" "dnd-copy"

# --- watch / wait (busy cursors) ---
# "wait" is the animated spinning beachball (primary busy cursor)
# "watch" is the static wristwatch — link common names to the animated one
make_link "wait" "progress"
make_link "wait" "half-busy"
# Also alias "watch" to "wait" so apps requesting "watch" get the beachball
make_link "wait" "watch"

# --- context_menu ---
make_link "context_menu" "right_ptr"

# --- pirate (poof / disappearing item) ---
make_link "pirate" "disappearing_item"
make_link "pirate" "X_cursor"

# --- dnd-link (alias/shortcut arrow) ---
# The animated version overwrites dnd-link if both exist;
# the static drag_link is a fallback. Link alias:
make_link "dnd-link" "alias"
make_link "dnd-link" "link"

# --- Diagonal resize combos (map to appropriate cursors) ---
# nw-se and ne-sw diagonal resize — map to the double arrows
make_link "sb_h_double_arrow" "nwse-resize"
make_link "sb_v_double_arrow" "nesw-resize"

# --- Additional common CSS cursor mappings ---
make_link "left_ptr" "left-arrow"
make_link "xterm" "vertical-text"
make_link "not-allowed" "not_allowed"
make_link "cell" "plus"

echo ""

# Go back to repo root
cd - > /dev/null

# ---------------------------------------------------------------------------
# Step 6: Write index.theme
# ---------------------------------------------------------------------------
# The index.theme file tells the X cursor loader the theme name and which
# theme to fall back to for any cursors we don't provide.

echo "--- Step 6: Writing index.theme ---"

cat > "$OUT_DIR/index.theme" << 'THEME'
[Icon Theme]
Name=SnowLeopard
Comment=Mac OS X Snow Leopard cursors for CopyCatOS
Inherits=Breeze_Light
THEME

echo "  Written to $OUT_DIR/index.theme"
echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

cursor_count=$(ls -1 "$OUT_DIR/cursors/" | wc -l | tr -d ' ')
symlink_count=$(find "$OUT_DIR/cursors/" -type l | wc -l | tr -d ' ')
file_count=$((cursor_count - symlink_count))

echo "=== Build complete ==="
echo "  Cursor files:  $file_count"
echo "  Symlinks:      $symlink_count"
echo "  Total entries: $cursor_count"
echo "  Output:        $OUT_DIR/"
echo ""
echo "To install system-wide:"
echo "  sudo cp -r $OUT_DIR /usr/share/icons/"
echo ""
echo "To install for current user:"
echo "  cp -r $OUT_DIR ~/.local/share/icons/"
echo ""
echo "To activate:"
echo "  gsettings set org.gnome.desktop.interface cursor-theme 'SnowLeopard'"
echo "  # or set in ~/.Xresources:"
echo "  # Xcursor.theme: SnowLeopard"
