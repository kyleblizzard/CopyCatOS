// CopyCatOS — by Kyle Blizzard at Blizzard.show

// debug.c — MOONBASE_DEBUG_JSON frame dumper.
//
// One JSON object per line, on stderr. Example output:
//
//   {"dir":"out","kind":"HELLO","kind_id":"0x0001","body":{...}}
//
// The body pretty-print walks the CBOR tree using cbor.h's reader.
// Unknown map keys are shown as "?N". Byte strings are rendered in
// lowercase hex. The dumper never allocates on the caller's behalf —
// it writes straight through stderr.

#include "debug.h"
#include "cbor.h"
#include "moonbase/ipc/kinds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// On/off decision — cached on first call
// ---------------------------------------------------------------------

static int g_debug_enabled = -1;   // -1 == unresolved, 0 == off, 1 == on

static bool debug_on(void) {
    if (g_debug_enabled < 0) {
        const char *v = getenv("MOONBASE_DEBUG_JSON");
        g_debug_enabled = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return g_debug_enabled == 1;
}

// ---------------------------------------------------------------------
// Kind id → name table
// ---------------------------------------------------------------------

static const char *kind_name(uint16_t kind) {
    switch (kind) {
        case MB_IPC_HELLO:                 return "HELLO";
        case MB_IPC_WELCOME:               return "WELCOME";
        case MB_IPC_BYE:                   return "BYE";
        case MB_IPC_ERROR:                 return "ERROR";
        case MB_IPC_PING:                  return "PING";
        case MB_IPC_PONG:                  return "PONG";
        case MB_IPC_WINDOW_CREATE:         return "WINDOW_CREATE";
        case MB_IPC_WINDOW_CREATE_REPLY:   return "WINDOW_CREATE_REPLY";
        case MB_IPC_WINDOW_CLOSE:          return "WINDOW_CLOSE";
        case MB_IPC_WINDOW_SHOW:           return "WINDOW_SHOW";
        case MB_IPC_WINDOW_SET_TITLE:      return "WINDOW_SET_TITLE";
        case MB_IPC_WINDOW_SET_SIZE:       return "WINDOW_SET_SIZE";
        case MB_IPC_WINDOW_SET_POSITION:   return "WINDOW_SET_POSITION";
        case MB_IPC_WINDOW_REQUEST_REDRAW: return "WINDOW_REQUEST_REDRAW";
        case MB_IPC_WINDOW_COMMIT:         return "WINDOW_COMMIT";
        case MB_IPC_WINDOW_SHOWN:          return "WINDOW_SHOWN";
        case MB_IPC_WINDOW_CLOSED:         return "WINDOW_CLOSED";
        case MB_IPC_WINDOW_RESIZED:        return "WINDOW_RESIZED";
        case MB_IPC_WINDOW_FOCUSED:        return "WINDOW_FOCUSED";
        case MB_IPC_WINDOW_REDRAW:         return "WINDOW_REDRAW";
        case MB_IPC_BACKING_SCALE_CHANGED: return "BACKING_SCALE_CHANGED";
        case MB_IPC_KEY_DOWN:              return "KEY_DOWN";
        case MB_IPC_KEY_UP:                return "KEY_UP";
        case MB_IPC_TEXT_INPUT:            return "TEXT_INPUT";
        case MB_IPC_POINTER_MOVE:          return "POINTER_MOVE";
        case MB_IPC_POINTER_DOWN:          return "POINTER_DOWN";
        case MB_IPC_POINTER_UP:            return "POINTER_UP";
        case MB_IPC_SCROLL:                return "SCROLL";
        case MB_IPC_TOUCH_BEGIN:           return "TOUCH_BEGIN";
        case MB_IPC_TOUCH_MOVE:            return "TOUCH_MOVE";
        case MB_IPC_TOUCH_END:             return "TOUCH_END";
        case MB_IPC_GESTURE_PINCH:         return "GESTURE_PINCH";
        case MB_IPC_GESTURE_SWIPE:         return "GESTURE_SWIPE";
        case MB_IPC_CONTROLLER_BUTTON:     return "CONTROLLER_BUTTON";
        case MB_IPC_CONTROLLER_AXIS:       return "CONTROLLER_AXIS";
        case MB_IPC_CONTROLLER_HOTPLUG:    return "CONTROLLER_HOTPLUG";
        case MB_IPC_CONSENT_REQUEST:       return "CONSENT_REQUEST";
        case MB_IPC_CONSENT_GRANT:         return "CONSENT_GRANT";
        case MB_IPC_CONSENT_DENY:          return "CONSENT_DENY";
        case MB_IPC_CLIPBOARD_CHANGED:     return "CLIPBOARD_CHANGED";
        case MB_IPC_THERMAL_CHANGED:       return "THERMAL_CHANGED";
        case MB_IPC_POWER_CHANGED:         return "POWER_CHANGED";
        case MB_IPC_LOW_MEMORY:            return "LOW_MEMORY";
        case MB_IPC_COLOR_SCHEME_CHANGED:  return "COLOR_SCHEME_CHANGED";
        default:                           return NULL;
    }
}

// ---------------------------------------------------------------------
// CBOR → JSON walker
// ---------------------------------------------------------------------
//
// The dumper is best-effort. Anything malformed is printed as "?bad"
// and the walker unwinds; we never crash on a garbage body.

static void fputesc_tstr(FILE *f, const char *s, size_t n) {
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void fputesc_hex(FILE *f, const uint8_t *b, size_t n) {
    fputc('"', f);
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
    fputc('"', f);
}

static void dump_item(FILE *f, mb_cbor_r_t *r);

static void dump_array(FILE *f, mb_cbor_r_t *r) {
    uint64_t n = 0;
    if (!mb_cbor_r_array_begin(r, &n)) { fputs("\"?bad\"", f); return; }
    fputc('[', f);
    for (uint64_t i = 0; i < n; i++) {
        if (i) fputc(',', f);
        dump_item(f, r);
    }
    fputc(']', f);
}

static void dump_map(FILE *f, mb_cbor_r_t *r) {
    uint64_t n = 0;
    if (!mb_cbor_r_map_begin(r, &n)) { fputs("\"?bad\"", f); return; }
    fputc('{', f);
    for (uint64_t i = 0; i < n; i++) {
        if (i) fputc(',', f);
        // Keys in our schemas are always uints, but print defensively.
        mb_cbor_ty_t kt = mb_cbor_r_peek(r);
        if (kt == MB_CBOR_TY_UINT) {
            uint64_t k = 0;
            mb_cbor_r_uint(r, &k);
            fprintf(f, "\"%" PRIu64 "\":", k);
        } else {
            fputs("\"?key\":", f);
            mb_cbor_r_skip(r);
        }
        dump_item(f, r);
    }
    fputc('}', f);
}

static void dump_item(FILE *f, mb_cbor_r_t *r) {
    mb_cbor_ty_t t = mb_cbor_r_peek(r);
    switch (t) {
        case MB_CBOR_TY_UINT: {
            uint64_t v = 0;
            mb_cbor_r_uint(r, &v);
            fprintf(f, "%" PRIu64, v);
            break;
        }
        case MB_CBOR_TY_NINT: {
            int64_t v = 0;
            mb_cbor_r_int(r, &v);
            fprintf(f, "%" PRId64, v);
            break;
        }
        case MB_CBOR_TY_TSTR: {
            const char *s = NULL; size_t n = 0;
            mb_cbor_r_tstr(r, &s, &n);
            fputesc_tstr(f, s, n);
            break;
        }
        case MB_CBOR_TY_BSTR: {
            const uint8_t *b = NULL; size_t n = 0;
            mb_cbor_r_bstr(r, &b, &n);
            fputesc_hex(f, b, n);
            break;
        }
        case MB_CBOR_TY_ARRAY: dump_array(f, r); break;
        case MB_CBOR_TY_MAP:   dump_map(f, r);   break;
        case MB_CBOR_TY_BOOL: {
            bool v = false;
            mb_cbor_r_bool(r, &v);
            fputs(v ? "true" : "false", f);
            break;
        }
        case MB_CBOR_TY_NULL:
            mb_cbor_r_null(r);
            fputs("null", f);
            break;
        case MB_CBOR_TY_FLOAT: {
            double v = 0.0;
            mb_cbor_r_float(r, &v);
            fprintf(f, "%g", v);
            break;
        }
        default:
            mb_cbor_r_skip(r);
            fputs("\"?\"", f);
            break;
    }
}

// ---------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------

void mb_debug_dump_frame(bool outgoing,
                         uint16_t kind,
                         const uint8_t *body, size_t body_len) {
    if (!debug_on()) return;

    const char *name = kind_name(kind);
    FILE       *f    = stderr;
    flockfile(f);
    fputs("{\"dir\":\"", f);
    fputs(outgoing ? "out" : "in", f);
    fputs("\",\"kind\":", f);
    if (name) {
        fputc('"', f); fputs(name, f); fputc('"', f);
    } else {
        fputs("null", f);
    }
    fprintf(f, ",\"kind_id\":\"0x%04x\",\"body\":", kind);

    if (body && body_len) {
        mb_cbor_r_t r;
        mb_cbor_r_init(&r, body, body_len);
        dump_item(f, &r);
    } else {
        fputs("null", f);
    }

    fputc('}', f);
    fputc('\n', f);
    funlockfile(f);
}
