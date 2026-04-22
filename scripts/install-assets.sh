#!/bin/bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# install-assets.sh
# Deploys CopyCatOS Snow Leopard assets from the source repository into the
# runtime directory (~/.local/share/aqua-widgets/) where the window manager,
# dock, menu bar, and other shell components expect to find them at launch.
#
# Safe to run multiple times — uses cp -n (no-clobber) by default so existing
# files are never overwritten. Pass --force to replace everything.

set -euo pipefail

# ---------------------------------------------------------------------------
# Color helpers — only emit ANSI codes when stdout is a terminal
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    RED='\033[0;31m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    GREEN='' YELLOW='' RED='' BOLD='' RESET=''
fi

info()    { printf "${GREEN}[OK]${RESET}  %s\n" "$*"; }
warn()    { printf "${YELLOW}[WARN]${RESET} %s\n" "$*"; }
err()     { printf "${RED}[ERR]${RESET}  %s\n" "$*" >&2; }
header()  { printf "\n${BOLD}── %s ──${RESET}\n" "$*"; }

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help)
            echo "Usage: $(basename "$0") [--force]"
            echo "  --force   Overwrite existing assets (default: skip existing)"
            exit 0
            ;;
        *)
            err "Unknown option: $arg"
            exit 1
            ;;
    esac
done

# cp flags: preserve timestamps, and skip existing unless --force
if [[ "$FORCE" -eq 1 ]]; then
    CP_FLAGS="-p"
else
    CP_FLAGS="-np"
fi

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# ASSET_SRC is the project root — one level above this script's directory
ASSET_SRC="$(cd "$(dirname "$0")/.." && pwd)"

# ASSET_DST is the runtime location the WM / shell reads at startup
ASSET_DST="$HOME/.local/share/aqua-widgets"

# Shorthand for the MacAssets tree inside the source repo
MAC="$ASSET_SRC/snowleopardaura/MacAssets"

# ---------------------------------------------------------------------------
# Validate source exists
# ---------------------------------------------------------------------------
if [[ ! -d "$MAC" ]]; then
    err "Source asset directory not found: $MAC"
    err "Make sure you are running this from within the CopyCatOS repository."
    exit 1
fi

# ---------------------------------------------------------------------------
# Counters for the summary
# ---------------------------------------------------------------------------
COPIED=0       # files actually written
SKIPPED=0      # files that already existed (no-clobber)
WARNINGS=0     # missing source files (non-fatal)
WARN_MSGS=()   # collect warning messages for the final summary

# copy_file SRC DST
#   Copies a single file. Increments the right counter. Warns (non-fatal) if
#   the source does not exist.
copy_file() {
    local src="$1" dst="$2"

    if [[ ! -f "$src" ]]; then
        warn "Source not found: $src"
        WARN_MSGS+=("Missing: $src")
        (( WARNINGS++ )) || true
        return
    fi

    # When not forcing, cp -n silently skips existing files. We detect that
    # by checking whether the destination already exists beforehand.
    if [[ -f "$dst" && "$FORCE" -eq 0 ]]; then
        (( SKIPPED++ )) || true
        return
    fi

    cp $CP_FLAGS "$src" "$dst"
    (( COPIED++ )) || true
}

# copy_dir SRC_DIR DST_DIR
#   Recursively copies an entire directory tree. Counts every file.
copy_dir() {
    local src_dir="$1" dst_dir="$2"

    if [[ ! -d "$src_dir" ]]; then
        warn "Source directory not found: $src_dir"
        WARN_MSGS+=("Missing dir: $src_dir")
        (( WARNINGS++ )) || true
        return
    fi

    # Walk every file in the source directory
    while IFS= read -r -d '' file; do
        # Build the matching destination path
        local rel="${file#"$src_dir"/}"
        local dest="$dst_dir/$rel"

        # Ensure the parent directory exists (handles nested subdirs)
        mkdir -p "$(dirname "$dest")"

        if [[ -f "$dest" && "$FORCE" -eq 0 ]]; then
            (( SKIPPED++ )) || true
        else
            cp $CP_FLAGS "$file" "$dest"
            (( COPIED++ )) || true
        fi
    done < <(find "$src_dir" -type f -print0)
}

# ---------------------------------------------------------------------------
# 1. Create runtime directory structure
# ---------------------------------------------------------------------------
header "Creating directory structure"

mkdir -p "$ASSET_DST"/{dock,menubar/extras,controls/{buttons,tracks,tabs,frames,segments,misc},materials,icons,wallpaper}
info "Directory tree created at $ASSET_DST"

# ---------------------------------------------------------------------------
# 2. Window Manager assets (traffic-light buttons)
# ---------------------------------------------------------------------------
header "Window Manager assets"

# The canonical traffic-light source is moonrock/assets/sl_*_button.png
# — 14x14 alpha-masked discs cropped from the real SL 10.6 screenshot
# at snowleopardaura/example photos/finderexample.png. MacAssets/Dock/
# closebox.png is a document-close (black background with X), not the
# titlebar traffic-light.
MOONROCK_BTN="$ASSET_SRC/moonrock/assets"

copy_file "$MOONROCK_BTN/sl_close_button.png"    "$ASSET_DST/sl_close_button.png"
copy_file "$MOONROCK_BTN/sl_minimize_button.png" "$ASSET_DST/sl_minimize_button.png"
copy_file "$MOONROCK_BTN/sl_zoom_button.png"     "$ASSET_DST/sl_zoom_button.png"

# ---------------------------------------------------------------------------
# 3. Dock assets
# ---------------------------------------------------------------------------
header "Dock assets"

# Named files
for f in scurve-xl.png scurve-l.png scurve-m.png scurve-sm.png \
         frontline.png \
         indicator_large.png indicator_medium.png indicator_small.png \
         separator.png separatorstraight.png \
         trashempty.png trashfull.png poof.png; do
    copy_file "$MAC/Dock/$f" "$ASSET_DST/dock/$f"
done

# Glob: all indicator_*_simple.png variants
for f in "$MAC/Dock"/indicator_*_simple.png; do
    [[ -f "$f" ]] || continue
    copy_file "$f" "$ASSET_DST/dock/$(basename "$f")"
done

# ---------------------------------------------------------------------------
# 3b. Stack popup assets (nine-patch backgrounds for folder stacks)
# ---------------------------------------------------------------------------
header "Stack popup assets"
mkdir -p "$ASSET_DST/dock/stacks"

# Aqua nine-patch popup background (13 pieces)
for f in eccl_top_left.png eccl_top.png eccl_top_right.png \
         eccl_left.png eccl_center.png eccl_right.png \
         eccl_bottom_left.png eccl_bottom.png eccl_bottom_right.png \
         eccl_callout_top.png eccl_callout_bottom.png \
         eccl_callout_left.png eccl_callout_right.png; do
    copy_file "$MAC/Dock/$f" "$ASSET_DST/dock/stacks/$f"
done

# Alternative stack backgrounds
for f in "$MAC/Dock"/stackbackground-*.png; do
    [ -f "$f" ] || continue
    copy_file "$f" "$ASSET_DST/dock/stacks/$(basename "$f")"
done

# Stack UI controls
for f in closebox.png closebox_pressed.png \
         back-button.png back-button-pressed.png \
         pileArrow.png pileLeftArrow.png pileRightArrow.png \
         stackitemshadow.png; do
    copy_file "$MAC/Dock/$f" "$ASSET_DST/dock/stacks/$f"
done

# Selection highlights and scrollbar parts for stack popups
for f in ecfl_selected.png ecfl_title_background.png ecfl_title_background_selected.png \
         ecm_checkmark.png ecm_checkmark_disabled.png \
         ecm_selection_blue.png ecm_selection_graphite.png \
         ecsbl_thumb_top.png ecsbl_thumb_center.png ecsbl_thumb_bottom.png \
         ecsbl_thumb_pressed_top.png ecsbl_thumb_pressed_center.png ecsbl_thumb_pressed_bottom.png \
         ecsbl_track_top.png ecsbl_track_center.png ecsbl_track_bottom.png; do
    copy_file "$MAC/Dock/$f" "$ASSET_DST/dock/stacks/$f"
done

# ---------------------------------------------------------------------------
# 4. Menu bar assets
# ---------------------------------------------------------------------------
header "Menu bar assets"

SNOW_REV="$MAC/SnowReverseOutput"

copy_file "$SNOW_REV/menubar/menubar_bg.png"        "$ASSET_DST/menubar/menubar_bg.png"
copy_file "$SNOW_REV/menubar/menu_bg.png"            "$ASSET_DST/menubar/menu_bg.png"
copy_file "$SNOW_REV/menubar/menuitem_selected.png"  "$ASSET_DST/menubar/menuitem_selected.png"

copy_file "$MAC/HIToolbox/HiResAppleMenu.png"          "$ASSET_DST/menubar/apple_logo.png"
copy_file "$MAC/HIToolbox/HiResAppleMenuGraphite.png"   "$ASSET_DST/menubar/apple_logo_graphite.png"
copy_file "$MAC/HIToolbox/HiResAppleMenuSelected.png"   "$ASSET_DST/menubar/apple_logo_selected.png"

# ---------------------------------------------------------------------------
# 5. Control assets (SnowReverseOutput sub-directories)
# ---------------------------------------------------------------------------
header "Control assets"

for subdir in buttons tracks tabs frames segments misc; do
    copy_dir "$SNOW_REV/$subdir" "$ASSET_DST/controls/$subdir"
done

# ---------------------------------------------------------------------------
# 6. Material textures (CoreUI)
# ---------------------------------------------------------------------------
header "Material textures"

COREUI="$MAC/CoreUI/UI.bundle/Contents/Resources"

for mat in aquamaterial.png brightmaterial.png glasshighlightmaterial.png \
           highlightmaterial.png inlaymaterial.png trackmaterial.png \
           redmaterial.png greenmaterial.png yellowmaterial.png \
           bluemap.png graphitemap.png; do
    copy_file "$COREUI/$mat" "$ASSET_DST/materials/$mat"
done

# A few CoreUI resources that belong under controls/
copy_file "$COREUI/resizestandard.png" "$ASSET_DST/controls/misc/resizestandard.png"
copy_file "$COREUI/progressramp.png"   "$ASSET_DST/controls/tracks/progressramp.png"
copy_file "$COREUI/checkmark.pdf"      "$ASSET_DST/controls/checkmark.pdf"

# ---------------------------------------------------------------------------
# 7. Default wallpaper
# ---------------------------------------------------------------------------
header "Wallpaper"

# Primary: Aurora.jpg from the Snow Leopard Nature wallpapers
AURORA="$MAC/Wallpapers/Nature/Aurora.jpg"
if [[ ! -f "$AURORA" ]]; then
    # Fallback: try a glob match
    shopt -s nullglob
    candidates=("$MAC"/Wallpapers/Aurora*.jpg "$MAC"/Wallpapers/*/Aurora*.jpg)
    shopt -u nullglob
    if [[ ${#candidates[@]} -gt 0 ]]; then
        AURORA="${candidates[0]}"
    fi
fi
copy_file "$AURORA" "$ASSET_DST/wallpaper/default.jpg"

# Optional 4K variant from the separate wallpaper directory
AURORA_4K="$ASSET_SRC/wallpaper/SnowLeopard/contents/images/3840x2160.jpg"
if [[ -f "$AURORA_4K" ]]; then
    copy_file "$AURORA_4K" "$ASSET_DST/wallpaper/aurora-4k.jpg"
else
    warn "4K Aurora wallpaper not found at $AURORA_4K"
    WARN_MSGS+=("Missing: 4K wallpaper (wallpaper/SnowLeopard/contents/images/3840x2160.jpg)")
    (( WARNINGS++ )) || true
fi

# ---------------------------------------------------------------------------
# 8. Font installation (Lucida Grande)
# ---------------------------------------------------------------------------
header "Fonts"

FONT_DIR="$HOME/.local/share/fonts"
mkdir -p "$FONT_DIR"

FONT_SRC="$ASSET_SRC/fonts/LucidaGrande.ttc"
FONT_DST="$FONT_DIR/LucidaGrande.ttc"

copy_file "$FONT_SRC" "$FONT_DST"

# Refresh the font cache so applications can find the new font immediately
if command -v fc-cache &>/dev/null; then
    fc-cache -f 2>/dev/null || true

    if fc-list 2>/dev/null | grep -qi "Lucida Grande"; then
        info "Lucida Grande registered in font cache"
    else
        warn "Lucida Grande copied but not detected by fc-list — font cache may need a manual refresh"
        WARN_MSGS+=("Font installed but not detected by fc-list")
        (( WARNINGS++ )) || true
    fi
else
    warn "fc-cache not found — font cache not updated (install fontconfig)"
    WARN_MSGS+=("fc-cache not available")
    (( WARNINGS++ )) || true
fi

# ---------------------------------------------------------------------------
# 9. System Preferences icons
# ---------------------------------------------------------------------------
header "System Preferences icons"

SYSPREFS_ICONS="$ASSET_SRC/systemcontrol/assets/icons"
SYSPREFS_ICONS_128="$ASSET_SRC/systemcontrol/assets/icons-128"

mkdir -p "$ASSET_DST/sysprefs/icons" "$ASSET_DST/sysprefs/icons-128"

# 32x32 pane icons (shown in the main icon grid)
copy_dir "$SYSPREFS_ICONS" "$ASSET_DST/sysprefs/icons"

# 128x128 pane icons (shown in the stub "not yet available" view)
copy_dir "$SYSPREFS_ICONS_128" "$ASSET_DST/sysprefs/icons-128"

# ---------------------------------------------------------------------------
# 10. Desktop shortcut .desktop files
# ---------------------------------------------------------------------------
header "Desktop shortcuts"

DESKTOP_DIR="$HOME/Desktop"
DESKTOP_SRC="$ASSET_SRC/scripts/desktop"

if [[ -d "$DESKTOP_SRC" ]]; then
    mkdir -p "$DESKTOP_DIR"
    for f in "$DESKTOP_SRC"/*.desktop; do
        [[ -f "$f" ]] || continue
        local_name="$(basename "$f")"
        copy_file "$f" "$DESKTOP_DIR/$local_name"
        # .desktop files must be executable for some launchers
        chmod +x "$DESKTOP_DIR/$local_name" 2>/dev/null || true
    done
else
    warn "Desktop shortcuts source not found: $DESKTOP_SRC"
    WARN_MSGS+=("Missing: scripts/desktop/ directory")
    (( WARNINGS++ )) || true
fi

# ---------------------------------------------------------------------------
# 11. Summary
# ---------------------------------------------------------------------------
header "Summary"

printf "  Files copied : ${GREEN}%d${RESET}\n" "$COPIED"
printf "  Skipped      : %d (already existed)\n" "$SKIPPED"
printf "  Warnings     : ${YELLOW}%d${RESET}\n" "$WARNINGS"
printf "  Total        : %d\n" $(( COPIED + SKIPPED ))

if [[ ${#WARN_MSGS[@]} -gt 0 ]]; then
    printf "\n${YELLOW}Warnings:${RESET}\n"
    for msg in "${WARN_MSGS[@]}"; do
        printf "  • %s\n" "$msg"
    done
fi

if [[ "$COPIED" -gt 0 || "$SKIPPED" -gt 0 ]]; then
    printf "\n${GREEN}Assets installed to ${BOLD}%s${RESET}\n" "$ASSET_DST"
else
    err "No files were installed — check warnings above."
    exit 1
fi
