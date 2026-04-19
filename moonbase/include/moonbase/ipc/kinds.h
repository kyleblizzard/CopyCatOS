// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase/ipc/kinds.h — canonical IPC kind enumeration.
//
// This header is the single source of truth for every u16 message kind
// spoken between `libmoonbase.so.1` and `moonrock`. IPC.md describes
// the wire format and per-kind body schema; this header encodes the
// kind numbers themselves so framework, compositor, and the debug
// dumper all agree on a single table at build time.
//
// Rules:
//   * Values are **stable**. Once assigned, a kind number never moves.
//   * New kinds append inside their reserved range (0x0100-block for
//     windows, 0x0200 for input, 0x0300 for consent, 0x0500 for
//     system signals). See IPC.md §4.
//   * Deleting or renumbering is an ABI break and requires
//     `libmoonbase.so.2`.
//
// The values here match IPC.md §5 exactly. If you change one and not
// the other, you have introduced a protocol bug.

#ifndef MOONBASE_IPC_KINDS_H
#define MOONBASE_IPC_KINDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t mb_ipc_kind_t;

enum {
    // ---- Lifecycle (0x0001 – 0x00FF) --------------------------------
    MB_IPC_HELLO                  = 0x0001,
    MB_IPC_WELCOME                = 0x0002,
    MB_IPC_BYE                    = 0x0003,
    MB_IPC_ERROR                  = 0x0004,
    MB_IPC_PING                   = 0x0005,
    MB_IPC_PONG                   = 0x0006,

    // ---- Windows (0x0100 – 0x01FF) ----------------------------------
    MB_IPC_WINDOW_CREATE          = 0x0100,
    MB_IPC_WINDOW_CREATE_REPLY    = 0x0101,
    MB_IPC_WINDOW_CLOSE           = 0x0102,
    MB_IPC_WINDOW_SHOW            = 0x0103,
    MB_IPC_WINDOW_SET_TITLE       = 0x0104,
    MB_IPC_WINDOW_SET_SIZE        = 0x0105,
    MB_IPC_WINDOW_SET_POSITION    = 0x0106,
    MB_IPC_WINDOW_REQUEST_REDRAW  = 0x0107,
    MB_IPC_WINDOW_COMMIT          = 0x0108,

    MB_IPC_WINDOW_SHOWN           = 0x0110,
    MB_IPC_WINDOW_CLOSED          = 0x0111,
    MB_IPC_WINDOW_RESIZED         = 0x0112,
    MB_IPC_WINDOW_FOCUSED         = 0x0113,
    MB_IPC_WINDOW_REDRAW          = 0x0114,
    MB_IPC_BACKING_SCALE_CHANGED  = 0x0115,

    // ---- Input (0x0200 – 0x02FF) ------------------------------------
    MB_IPC_KEY_DOWN               = 0x0200,
    MB_IPC_KEY_UP                 = 0x0201,
    MB_IPC_TEXT_INPUT             = 0x0202,

    MB_IPC_POINTER_MOVE           = 0x0210,
    MB_IPC_POINTER_DOWN           = 0x0211,
    MB_IPC_POINTER_UP             = 0x0212,
    MB_IPC_SCROLL                 = 0x0213,

    MB_IPC_TOUCH_BEGIN            = 0x0220,
    MB_IPC_TOUCH_MOVE             = 0x0221,
    MB_IPC_TOUCH_END              = 0x0222,

    MB_IPC_GESTURE_PINCH          = 0x0230,
    MB_IPC_GESTURE_SWIPE          = 0x0231,

    MB_IPC_CONTROLLER_BUTTON      = 0x0240,
    MB_IPC_CONTROLLER_AXIS        = 0x0241,
    MB_IPC_CONTROLLER_HOTPLUG     = 0x0242,

    MB_IPC_DRAG_ENTER             = 0x0250,
    MB_IPC_DRAG_OVER              = 0x0251,
    MB_IPC_DRAG_LEAVE             = 0x0252,
    MB_IPC_DRAG_DROP              = 0x0253,

    // ---- Consent / permissions (0x0300 – 0x03FF) --------------------
    MB_IPC_CONSENT_REQUEST        = 0x0300,
    MB_IPC_CONSENT_GRANT          = 0x0301,
    MB_IPC_CONSENT_DENY           = 0x0302,

    // ---- 0x0400 – 0x04FF: reserved (future app-to-app / prefs) ------
    // ---- 0x0500 – 0x05FF: system signals ----------------------------
    MB_IPC_CLIPBOARD_CHANGED      = 0x0500,
    MB_IPC_THERMAL_CHANGED        = 0x0501,
    MB_IPC_POWER_CHANGED          = 0x0502,
    MB_IPC_LOW_MEMORY             = 0x0503,
    MB_IPC_COLOR_SCHEME_CHANGED   = 0x0504,

    // ---- 0x0600 – 0x06FF: reserved (future metrics feed) ------------
    // ---- 0x0700 – 0x07FF: reserved (future moonbase_display.h) ------
    // ---- 0xFF00 – 0xFFFF: reserved for debug / internal -------------
};

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_KINDS_H
