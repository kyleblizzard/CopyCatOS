# MoonRock Desktop + Finder Roadmap
# Last updated: April 16, 2026

---

## Honest Starting Point

MoonRock is approximately 12% of the way to a real Snow Leopard desktop-icons + Finder
experience. The compositor substrate is excellent — GPU-accelerated, damage-driven,
Snow Leopard-spec shadows, real genie animation framework, proper Spaces + Exposé
separate (not combined like Mission Control). But that is the canvas, not the painting.

The desktop-icons layer is a basic skeleton. A real Finder equivalent does not exist at all.
Spatial memory is unimplemented. Color labels are unimplemented. XDND is unimplemented.
Volume management on the desktop is unimplemented. Quick Look is unimplemented.
Zero folder windows exist.

Reference: screenshots taken April 16 2026 from:
- Real Snow Leopard machine (192.168.68.74) — saved /tmp/sl_desktop.png
- Nobara/Lenovo Legion Go (192.168.68.75) — saved /tmp/nobara_current.png

---

## Architecture Decisions (Locked)

- **cc-finder**: Single persistent daemon process managing multiple windows.
  Mirrors how real Finder.app works. Benefits: shared icon cache, shared thumbnail
  cache, spatial memory loaded once, O(1) cost for subsequent windows.
- **All shell code**: Pure C + Cairo + Pango + Xlib. No FLTK, no GTK, no GNOME, no KDE.
- **UI from real assets**: Every visual element sourced from snowleopardaura/MacAssets/
  using the same extraction approach as the rest of the shell.
- **Framework philosophy**: We build our own micro-frameworks from real Snow Leopard
  assets + osxguidelines in snowleopardaura/. No approximations. No "close enough."

---

## Current State Audit (April 16, 2026)

### What's Working

| Feature | Status | Notes |
|---|---|---|
| Drop shadows (Snow Leopard spec) | DONE | 20px blur, 3px Y-offset, 0.40 alpha |
| Frosted glass blur-behind | DONE | FBO 2-pass Gaussian |
| Genie minimize shader | DONE | Mesh distortion vertex shader exists |
| Genie — wired to minimize button | MISSING | Framework done, not connected to button |
| GPU compositing pipeline | DONE | Zero-copy texture_from_pixmap, damage-driven |
| Spaces + Exposé (SEPARATE) | DONE | Not combined like Mission Control |
| Space thumbnails in overview | DONE | FBO-rendered 200x125px |
| Dock bounce, magnification, reflections | DONE | |
| Dock stacks with live preview | DONE | |
| Global menu bar with real app menu | DONE | |
| Menu bar strut reservation | DONE | |
| Window decorations (title bar, traffic lights) | DONE | decor.c 349 lines, PNG button assets |
| Desktop icon grid with inotify | DONE | Grid exists |
| Desktop drag-to-reposition | DONE | Local reposition only |
| Right-click context menu on desktop | PARTIAL | Missing: Label submenu, Get Info |
| Double-click to open | DONE | fork+exec xdg-open |
| EWMH compliance (26 atoms) | DONE | |
| Resize handles (edge/corner) | DONE | resize.c |

### What's Completely Missing

| Feature | Priority |
|---|---|
| Genie animation wired to minimize button | HIGH |
| Spatial memory — icon positions persist across sessions | HIGH |
| Color labels (xattr-based, full row highlight) | HIGH |
| XDND — cross-window drag-and-drop protocol | HIGH |
| Persistent scroll bars (not auto-hide) | HIGH |
| cc-finder — Finder window equivalent | HIGH |
| Finder window state persistence per directory | HIGH |
| Quick Look overlay (Spacebar preview) | HIGH |
| Trash icon with badge count | MEDIUM |
| Volume management on desktop (udisks2) | MEDIUM |
| Spring-loaded folders | MEDIUM |
| Get Info panel | MEDIUM |
| Icon badge overlays (alias arrow, eject, unsaved) | MEDIUM |
| Launch Services daemon (socket-based MIME routing) | MEDIUM |
| Alias creation via drag (Alt+Super modifier) | LOW |
| Volume drag to trash = eject | LOW |
| Status bar sort/grid indicator | LOW |
| Bonjour/Avahi sidebar discovery | LOW |
| Damage-driven partial repaint for desktop | LOW |

---

## Reference Material

These articles are the source of truth for what behaviors matter. Read them before
implementing anything and cross-check every behavior against them:

- Riccardo Mori — "A retrospective look at Mac OS X Snow Leopard":
  https://morrick.me/archives/9220
- Riccardo Mori — Addendum: https://morrick.me/archives/9246
- Riccardo Mori — "The reshaped Mac experience": https://morrick.me/archives/9150
- probonopd — "Make. It. Simple. Linux Desktop Usability" series (6 parts):
  https://medium.com/@probonopd/make-it-simple-linux-desktop-usability-part-1-5fa0fb369b42

Key criticisms they raise that we MUST fix:

From Mori:
- Icons must stay exactly where you put them (spatial memory)
- Scroll bars must always be visible — their length tells you how much content exists
- Color labels must color the ENTIRE selection rectangle (not dots like Tags)
- Exposé and Spaces must stay SEPARATE — Mission Control's merger was a regression
- Status bar bottom-left icon showing sort/grid/read-only state

From probonopd:
- Desktop icons must exist and work (GNOME tried to remove them — catastrophic)
- File associations must just work (no manual text config editing)
- Drag-and-drop must work across all windows
- Direct manipulation must feel immediate — animations communicate cause/effect
- Double-click to open must work predictably

---

## Implementation Plans

### GENIE ANIMATION — Wire to Minimize Button

**Status:** PARTIAL (shader exists, not connected)

**Why X11 doesn't provide this:** X11 knows nothing about minimize semantics. The WM
must intercept WM_CHANGE_STATE (minimize request), suppress the actual unmap, run the
animation, then unmap. Nothing in X11 does this automatically.

**Files:**
- cc-wm/src/events.c — handle_client_message(): intercept WM_CHANGE_STATE ICONIC
- cc-wm/src/frame.c — trigger moonrock_start_genie() before XUnmapWindow
- cc-dock/src/ — add dock_get_icon_rect(WM_CLASS) via _CC_DOCK_ICON_RECT atom

**Approach:**
```c
// events.c — handle_client_message()
case WM_CHANGE_STATE with ICONIC:
  if (!client->anim_in_progress) {
      client->anim_in_progress = 1;
      XPoint dest = dock_get_icon_center(wm, client->class);
      moonrock_start_genie(client, GENIE_MINIMIZE, dest);
      // completion callback: XUnmapWindow(), client->is_minimized = 1
  }
```

---

### XDND — X Drag and Drop Protocol

**Status:** MISSING — the most critical missing primitive for direct manipulation.

**Why X11 doesn't provide this:** X11's selection mechanism is asynchronous and requires
explicit protocol participation. XDND v5 is 14 atoms and a multi-step handshake.

**Atoms to add (cc-wm/src/ewmh.c):**
```
XdndAware, XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop,
XdndFinished, XdndActionCopy, XdndActionMove, XdndActionLink,
XdndTypeList, XdndSelection, XdndProxy
```

**Source role (when dragging FROM desktop/finder):**
1. Set XdndAware property on window (version 5)
2. On mouse press+move: find target window via XQueryPointer
3. Send XdndEnter to target with offered MIME types
4. Send XdndPosition on each mouse move
5. Receive XdndStatus (accept/reject)
6. On release: send XdndDrop, receive XdndFinished
7. Transfer data via XConvertSelection(dpy, XdndSelection, ...)

**Target role (dropping ONTO desktop):**
- Set XdndProxy on root window → all desktop drops forward to cc-desktop
- Handle XdndPosition: compute target icon under cursor, send XdndStatus
- Handle XdndDrop: copy/move file via rename()/copy_file()

**Files:**
- cc-desktop/src/dnd.c (new, ~500 LOC)
- cc-wm/src/ewmh.c (add atom interns)
- cc-dock/src/dnd.c (extend with XDND target for file drops onto dock icons)

---

### SPATIAL MEMORY — Persistent Icon Layout

**Status:** MISSING

**Why X11 doesn't provide this:** X11 is completely stateless across sessions.

**Storage:** ~/.local/share/copycatos/desktop-layout.json

Format:
```json
{
  "Desktop/2024-taxes.pdf": {"x": 1234, "y": 56, "custom_placed": true},
  "Desktop/Projects/":      {"x": 1144, "y": 56, "custom_placed": true}
}
```

**Behavioral rules (must match Snow Leopard exactly):**
- Custom-placed icons never move unless user moves them
- "Clean Up" snaps to grid but preserves relative order
- "Sort by Name" does stable sort and saves new positions
- New files: find next empty grid cell (top-right origin, column-major)
- Collision (external tool created file): offset by one cell

**Files:**
- cc-desktop/src/layout.c (new, ~300 LOC)
- cc-desktop/src/icons.c (load on startup, save on drag complete)

---

### COLOR LABELS

**Status:** MISSING

**Why X11 doesn't provide this:** Filesystem has no label concept. Use xattr.

**Storage:** user.copycatos.label xattr on each file
Values: "", "red", "orange", "yellow", "green", "blue", "purple", "grey"

**Colors (full selection rect, 80% cell width, opaque — NOT colored dots):**
- Red:    #FF6C6C
- Orange: #FFB347
- Yellow: #FFFF6C
- Green:  #6CFF6C
- Blue:   #6C6CFF
- Purple: #CF6CFF
- Grey:   #B0B0B0

**Mori specifically: labels color the ENTIRE name rect, making items stand out.
Tags (dots) are a regression. We implement labels, not tags.**

**Context menu in cc-desktop:** right-click → Label ▶ [colored circles] None/Red/.../Grey

**In cc-finder (list view):** entire row background at 40% opacity
**In cc-finder (icon view):** selection rect behind icon at full opacity

**Files:**
- cc-desktop/src/labels.c (new, ~200 LOC)
- cc-desktop/src/icons.c (read xattr on load, draw colored rect before icon)
- cc-desktop/src/contextmenu.c (label submenu)

---

### PERSISTENT SCROLL BARS

**Status:** MISSING

**Why it matters:** Mori states explicitly: "The scroll bar length indicates current
position within content — essential information whether actively scrolling or not."
Hidden scroll bars (macOS post-Lion, most Linux desktops) are a regression.

**For cc-finder (all views):** Draw scroll bars as permanent 15px strips.
- Track: light grey
- Thumb: proportional to viewport/content ratio
- Thumb color: blue (#3875D7) when dragging, grey (#B0B0B0) when idle
- Always visible. No fade. No auto-hide.

**For breeze-aqua Qt style:** styleHint(SH_ScrollBar_Transient) must return false.

---

### cc-finder — Native C Finder Equivalent

**Status:** MISSING — the largest single gap.

**Architecture:** Single persistent daemon process (mirrors real Finder.app).
All Finder windows live in one process. Shared: icon cache, thumbnail cache,
spatial memory loaded once, sidebar state.

**File layout:**
```
cc-finder/src/
├── main.c          (~150 LOC) arg parsing, daemon mode, initial window
├── finder.c/h      (~600 LOC) top-level state, window list, IPC socket
├── toolbar.c/h     (~400 LOC) Back/Forward, view buttons, Action, Search
├── sidebar.c/h     (~700 LOC) Devices/Shared/Places/Search sections
├── iconview.c/h    (~800 LOC) icon grid with spatial memory per directory
├── listview.c/h    (~700 LOC) sortable columns (Name, Date, Size, Kind, Label)
├── columnview.c/h  (~600 LOC) Miller columns layout
├── pathbar.c/h     (~300 LOC) clickable path segments at bottom
├── statusbar.c/h   (~200 LOC) "N items, X.X GB available"
├── navigation.c/h  (~300 LOC) history stack (Back/Forward)
├── dnd.c/h         (~500 LOC) XDND source + target for finder windows
├── labels.c/h      (~200 LOC) xattr read/write, color rendering (shared with desktop)
├── getinfo.c/h     (~400 LOC) Cmd+I panel
├── spatial.c/h     (~350 LOC) per-directory layout JSON persistence
├── search.c/h      (~450 LOC) live search
├── preview.c/h     (~300 LOC) icon thumbnail generation
└── volume.c/h      (~400 LOC) udisks2 D-Bus: mount/unmount/eject
```

**Estimated total:** ~7,000-9,000 LOC

**Sidebar sections (Snow Leopard exact):**
- DEVICES: Hard disks, external drives, CDs/DVDs (udisks2), network volumes
- SHARED: Bonjour/Avahi discovered AFP/SMB shares
- PLACES: Desktop, home, Applications, Documents, Downloads + user-draggable
- SEARCH FOR: Today, Yesterday, Past Week, All Images, All Movies, All Documents

**Window state persistence per directory:**
~/.local/share/copycatos/finder/{sha256(path)}.json
```json
{
  "view": "icon", "icon_size": 64,
  "sort_by": "name", "sort_direction": "asc",
  "window_x": 200, "window_y": 150, "window_w": 780, "window_h": 480,
  "sidebar_width": 180, "scroll_x": 0, "scroll_y": 0
}
```

**IPC atoms (ClientMessage from other shell components):**
- _CC_FINDER_OPEN_PATH — dock "Show In Finder" → navigate to path
- _CC_FINDER_REVEAL_FILE — select + scroll to specific file
- _CC_QUICKLOOK_REQUEST — forward Spacebar to cc-quicklook

---

### QUICK LOOK

**Status:** MISSING

**MIME dispatch:**
- Images: libpng, libjpeg-turbo, libwebp → Cairo surface → scale to fit
- Text: Pango render (monospace, line wrap, line numbers)
- PDF: poppler-glib → page 1 as Cairo surface
- Video: ffmpeg av_seek_frame() to 25% → extract frame → Cairo

**Chrome:** Snow Leopard Quick Look window — white rounded rect, drop shadow,
nav arrows for multi-file, filename + dimensions in bottom strip.

**IPC:** cc-desktop sends _CC_QUICKLOOK_OPEN ClientMessage with path in XChangeProperty.
Spacebar in cc-desktop/cc-finder calls this.

---

### TRASH ICON + BADGE

**Status:** MISSING

**Standard:** freedesktop.org Trash spec (~/.local/share/Trash/)
- Trash icons: EmptyTrash.png / FullTrash.png from snowleopardaura/MacAssets/Icons/
- Badge: count .trashinfo files in ~/.local/share/Trash/info/
- Position: hardcoded lower-right of desktop, separate from icon grid
- XDND target: drops move files to trash (rename + write .trashinfo)
- Right-click: "Empty Trash" → confirmation → unlink all

---

### VOLUME MANAGEMENT ON DESKTOP

**Status:** MISSING

**Approach:** D-Bus + udisks2
- Watch org.freedesktop.UDisks2 for DeviceAdded/DeviceRemoved signals
- Display removable volumes as icons in top-right zone (separate from ~/Desktop grid)
- Eject via right-click → org.freedesktop.UDisks2.Drive.Eject()
- Volume drag to Trash → eject (not delete)

---

### LAUNCH SERVICES DAEMON

**Status:** MISSING (currently using xdg-open shell script)

**Why it matters:** probonopd Part 6: "Unlike macOS's automatic Launch Services
database, Linux requires users to manually edit text configuration files."

**Design:**
- cc-launchservices daemon (~500 LOC)
- Watches /usr/share/applications/, ~/.local/share/applications/, AppImage mounts
- Builds MIME-type → app mapping persisted to ~/.local/share/copycatos/lsdb.json
- Unix socket IPC: {"action": "open", "path": "/tmp/foo.pdf"} → launch correct app
- Per-user overrides for "Open With..." preference
- cc-desktop and cc-finder both query this instead of xdg-open

---

## Development Roadmap

### Phase 1 — Direct Manipulation Foundation (2-3 weeks)
*Goal: Desktop icons behave correctly. Icons survive restarts. Drag works everywhere.*

1. [ ] Wire genie animation to minimize button (events.c + dock IPC)
2. [ ] Spatial memory persistence (cc-desktop/src/layout.c, ~300 LOC new file)
3. [ ] Color labels (cc-desktop/src/labels.c + contextmenu.c + icons.c)
4. [ ] XDND protocol atoms + cc-desktop source/target roles
5. [ ] Trash icon (position, inotify badge, XDND target)

Milestone: Desktop icons survive restart. Color labels work. Files can be dragged
from desktop to application windows. Trash works.

### Phase 2 — cc-finder (6-8 weeks)
*Goal: A real Finder that opens folders, maintains spatial memory, has sidebar + views.*

Build order:
1. [ ] Basic window + icon view (hardcoded home directory) — 2 weeks
2. [ ] Sidebar (Places + Devices) + udisks2 volume.c — 1 week
3. [ ] Navigation (Back/Forward history) + path bar — 1 week
4. [ ] List view + Column view — 1.5 weeks
5. [ ] Window state persistence per directory — 0.5 week
6. [ ] XDND source + target in finder windows — 0.5 week
7. [ ] Labels in all views — 0.5 week
8. [ ] Search — 0.5 week

Milestone: Can navigate filesystem in a window that looks/behaves like Snow Leopard Finder.

### Phase 3 — Direct Manipulation Details (3-4 weeks)
*Goal: Every Mori + probonopd critique is addressed*

1. [ ] Quick Look (cc-quicklook/)
2. [ ] Spring-loaded folders (500ms hover timer in XDND position handler)
3. [ ] Get Info panel (floating, per-file, stackable)
4. [ ] Launch Services daemon (cc-launchservices/)
5. [ ] Persistent scroll bars in cc-finder + breeze-aqua
6. [ ] Status bar sort/grid indicator
7. [ ] Bonjour/Avahi sidebar discovery
8. [ ] Alias creation via drag (Alt+Super modifier)

Milestone: CopyCatOS passes every behavioral test Mori and probonopd document as
broken on Linux.

---

## Color Reference (from real Snow Leopard screenshots)

```
Active title bar gradient:
  y=0:  RGB(76, 125, 176)   blue-gray top  [Finder-specific windows only]
  y=9:  RGB(73, 118, 167)   slightly darker
  y=10: RGB(62, 101, 143)   dark blue divider
  y=11: RGB(226, 226, 226)  jump to light gray
  y=19: RGB(208, 208, 208)  mid gray
  y=21: RGB(202, 202, 202)  bottom

Standard window active gradient (Terminal, TextEdit, etc.):
  top:  RGB(212, 212, 212)  — matches decor.c current implementation
  mid:  RGB(196, 196, 196)
  bot:  RGB(172, 172, 172)

Inactive title bar: RGB(238,238,238) -> RGB(220,220,220)
Title bar bottom border: active RGB(140,140,140), inactive RGB(185,185,185)

Traffic lights: Close #FF5F57, Minimize #FFBD2E, Zoom #28C940
Inactive buttons: #B0B0B0

Selection blue: #3875D7
Focus ring: #3B99FC
Window background: #ECECEC
Content area: #FFFFFF
Sidebar: #D8DEE8
Menu bar gradient: #F2F2F2 -> #E8E8E8 -> #D7D7D7 -> #D2D2D2
Menu bar bottom border: #A8A8A8

Window shadow: RGBA(0,0,0,0.4), 20px blur, 3px Y-offset
Window border: active RGB(160,160,160), inactive RGB(190,190,190)

Text primary: #1A1A1A
Text secondary: #737373
Text disabled: #A6A6A6
Text on selection: #FFFFFF

Color label rects (full opaque rect, 80% cell width):
  Red:    #FF6C6C
  Orange: #FFB347
  Yellow: #FFFF6C
  Green:  #6CFF6C
  Blue:   #6C6CFF
  Purple: #CF6CFF
  Grey:   #B0B0B0
```

---

## Asset Locations for Desktop/Finder Work

| What | Path |
|------|------|
| Finder window assets | snowleopardaura/MacAssets/Finder/ |
| Scroll bar assets | snowleopardaura/MacAssets/SnowReverseOutput/tracks/ |
| Button assets | snowleopardaura/MacAssets/SnowReverseOutput/buttons/ |
| Frame assets | snowleopardaura/MacAssets/SnowReverseOutput/frames/ |
| Sidebar icons | snowleopardaura/MacAssets/SnowReverseOutput/misc/ |
| Toolbar assets | snowleopardaura/MacAssets/SnowReverseOutput/misc/ |
| System icons | snowleopardaura/MacAssets/Icons/ |
| Trash icons | snowleopardaura/MacAssets/Icons/ (look for Trash.icns) |
| CoreUI materials | snowleopardaura/MacAssets/CoreUI/UI.bundle/Contents/Resources/ |
