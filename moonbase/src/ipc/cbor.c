// CopyCatOS — by Kyle Blizzard at Blizzard.show

// cbor.c — minimum CBOR encoder + decoder (see cbor.h for scope).
//
// Encoding rules used by this subset:
//   initial byte = (major_type << 5) | info
//   info in 0..23  -> argument is info itself
//   info == 24     -> 1-byte argument follows
//   info == 25     -> 2-byte BE argument
//   info == 26     -> 4-byte BE argument
//   info == 27     -> 8-byte BE argument
//
// Major types used:
//   0: unsigned int
//   1: negative int (value = -1 - argument)
//   2: byte string (argument = length)
//   3: text string (argument = length, UTF-8)
//   4: array (argument = count)
//   5: map (argument = pair count)
//   7: special (simple 20=false 21=true 22=null 26=float32 27=float64)
//
// Everything outside that subset sets MB_EPROTO on the decoder. The
// encoder always produces the shortest head that fits the argument,
// which matches CBOR "deterministic encoding" head rules.

#include "cbor.h"
#include "CopyCatAppKit.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// Big-endian primitives
// ---------------------------------------------------------------------

static inline void be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline void be64(uint8_t *p, uint64_t v) {
    be32(p,     (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

static inline uint16_t ld_be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static inline uint32_t ld_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
         | (uint32_t)p[2] << 8  | (uint32_t)p[3];
}
static inline uint64_t ld_be64(const uint8_t *p) {
    return ((uint64_t)ld_be32(p) << 32) | ld_be32(p + 4);
}

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

static bool w_fail(mb_cbor_w_t *w, int err) {
    if (!w->err) w->err = err;
    return false;
}

void mb_cbor_w_init_fixed(mb_cbor_w_t *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->owns_buf = false;
    w->err = 0;
}

void mb_cbor_w_init_grow(mb_cbor_w_t *w, size_t initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    w->buf = (uint8_t *)malloc(initial_cap);
    w->cap = w->buf ? initial_cap : 0;
    w->len = 0;
    w->owns_buf = w->buf != NULL;
    w->err = w->buf ? 0 : MB_ENOMEM;
}

void mb_cbor_w_drop(mb_cbor_w_t *w) {
    if (w->owns_buf) free(w->buf);
    w->buf = NULL;
    w->cap = 0;
    w->len = 0;
    w->owns_buf = false;
    w->err = 0;
}

uint8_t *mb_cbor_w_finish(mb_cbor_w_t *w, size_t *out_len) {
    if (w->err) {
        mb_cbor_w_drop(w);
        if (out_len) *out_len = 0;
        return NULL;
    }
    uint8_t *buf = w->buf;
    if (out_len) *out_len = w->len;
    // Ownership transfers to the caller regardless of how we
    // allocated; a caller who started with init_fixed already owns
    // the buffer and gets the same pointer back.
    w->buf = NULL;
    w->cap = 0;
    w->len = 0;
    w->owns_buf = false;
    return buf;
}

bool mb_cbor_w_ok(const mb_cbor_w_t *w) { return w->err == 0; }
int  mb_cbor_w_err(const mb_cbor_w_t *w) { return w->err; }

// Reserve n more bytes in the writer. Grows the buffer if growable,
// flags MB_ENOMEM / overflow otherwise.
static bool w_reserve(mb_cbor_w_t *w, size_t n) {
    if (w->err) return false;
    if (w->len + n < w->len) return w_fail(w, MB_EINVAL);   // overflow
    if (w->len + n <= w->cap) return true;
    if (!w->owns_buf) return w_fail(w, MB_ENOMEM);
    size_t new_cap = w->cap ? w->cap : 64;
    while (new_cap < w->len + n) {
        if (new_cap > SIZE_MAX / 2) return w_fail(w, MB_ENOMEM);
        new_cap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(w->buf, new_cap);
    if (!nb) return w_fail(w, MB_ENOMEM);
    w->buf = nb;
    w->cap = new_cap;
    return true;
}

// Write the CBOR "head": initial byte + argument encoded in the
// shortest form possible. Used by every major type.
static void w_head(mb_cbor_w_t *w, uint8_t major, uint64_t arg) {
    if (w->err) return;
    uint8_t major_shifted = (uint8_t)(major << 5);
    if (arg < 24) {
        if (!w_reserve(w, 1)) return;
        w->buf[w->len++] = major_shifted | (uint8_t)arg;
    } else if (arg <= 0xFF) {
        if (!w_reserve(w, 2)) return;
        w->buf[w->len++] = major_shifted | 24;
        w->buf[w->len++] = (uint8_t)arg;
    } else if (arg <= 0xFFFF) {
        if (!w_reserve(w, 3)) return;
        w->buf[w->len++] = major_shifted | 25;
        be16(w->buf + w->len, (uint16_t)arg);
        w->len += 2;
    } else if (arg <= 0xFFFFFFFFu) {
        if (!w_reserve(w, 5)) return;
        w->buf[w->len++] = major_shifted | 26;
        be32(w->buf + w->len, (uint32_t)arg);
        w->len += 4;
    } else {
        if (!w_reserve(w, 9)) return;
        w->buf[w->len++] = major_shifted | 27;
        be64(w->buf + w->len, arg);
        w->len += 8;
    }
}

void mb_cbor_w_uint(mb_cbor_w_t *w, uint64_t v) {
    w_head(w, 0, v);
}

void mb_cbor_w_int(mb_cbor_w_t *w, int64_t v) {
    if (v >= 0) {
        w_head(w, 0, (uint64_t)v);
    } else {
        // CBOR encoding of negative: -1 - n  =>  argument = -(v+1)
        uint64_t arg = (uint64_t)(-(v + 1));
        w_head(w, 1, arg);
    }
}

void mb_cbor_w_bool(mb_cbor_w_t *w, bool v) {
    w_head(w, 7, v ? 21u : 20u);
}

void mb_cbor_w_null(mb_cbor_w_t *w) {
    w_head(w, 7, 22u);
}

// Convert a double to IEEE 754 float32 big-endian bytes. We always
// emit float32 unless the caller explicitly asks for float64 — the
// protocol has no fields that need more than single precision.
void mb_cbor_w_float(mb_cbor_w_t *w, double v) {
    if (w->err) return;
    if (!w_reserve(w, 5)) return;
    w->buf[w->len++] = (7u << 5) | 26u;   // major 7, info 26 = float32
    float f = (float)v;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    be32(w->buf + w->len, bits);
    w->len += 4;
}

void mb_cbor_w_float64(mb_cbor_w_t *w, double v) {
    if (w->err) return;
    if (!w_reserve(w, 9)) return;
    w->buf[w->len++] = (7u << 5) | 27u;   // major 7, info 27 = float64
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    be64(w->buf + w->len, bits);
    w->len += 8;
}

void mb_cbor_w_tstr_n(mb_cbor_w_t *w, const char *s, size_t n) {
    w_head(w, 3, (uint64_t)n);
    if (!w_reserve(w, n)) return;
    if (n) memcpy(w->buf + w->len, s, n);
    w->len += n;
}

void mb_cbor_w_tstr(mb_cbor_w_t *w, const char *s) {
    mb_cbor_w_tstr_n(w, s, s ? strlen(s) : 0);
}

void mb_cbor_w_bstr(mb_cbor_w_t *w, const uint8_t *b, size_t n) {
    w_head(w, 2, (uint64_t)n);
    if (!w_reserve(w, n)) return;
    if (n) memcpy(w->buf + w->len, b, n);
    w->len += n;
}

void mb_cbor_w_array_begin(mb_cbor_w_t *w, size_t count) {
    w_head(w, 4, (uint64_t)count);
}

void mb_cbor_w_map_begin(mb_cbor_w_t *w, size_t count) {
    w_head(w, 5, (uint64_t)count);
}

// ---------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------

static bool r_fail(mb_cbor_r_t *r, int err) {
    if (!r->err) r->err = err;
    return false;
}

void mb_cbor_r_init(mb_cbor_r_t *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

bool mb_cbor_r_ok(const mb_cbor_r_t *r) { return r->err == 0; }
int  mb_cbor_r_err(const mb_cbor_r_t *r) { return r->err; }
bool mb_cbor_r_at_end(const mb_cbor_r_t *r) { return r->pos >= r->len; }

// Read the CBOR head at r->pos. On success, *out_major is 0..7 and
// *out_arg is the raw argument (length/count/value). Does not
// validate the argument against the major type.
static bool r_head(mb_cbor_r_t *r, uint8_t *out_major, uint64_t *out_arg) {
    if (r->err) return false;
    if (r->pos >= r->len) return r_fail(r, MB_EPROTO);
    uint8_t b = r->buf[r->pos++];
    uint8_t major = b >> 5;
    uint8_t info  = b & 0x1F;
    uint64_t arg = 0;
    if (info < 24) {
        arg = info;
    } else if (info == 24) {
        if (r->pos + 1 > r->len) return r_fail(r, MB_EPROTO);
        arg = r->buf[r->pos];
        r->pos += 1;
    } else if (info == 25) {
        if (r->pos + 2 > r->len) return r_fail(r, MB_EPROTO);
        arg = ld_be16(r->buf + r->pos);
        r->pos += 2;
    } else if (info == 26) {
        if (r->pos + 4 > r->len) return r_fail(r, MB_EPROTO);
        arg = ld_be32(r->buf + r->pos);
        r->pos += 4;
    } else if (info == 27) {
        if (r->pos + 8 > r->len) return r_fail(r, MB_EPROTO);
        arg = ld_be64(r->buf + r->pos);
        r->pos += 8;
    } else {
        // info 28..30 reserved; info 31 is the indefinite-length
        // "break" marker, which this subset does not accept.
        return r_fail(r, MB_EPROTO);
    }
    *out_major = major;
    *out_arg = arg;
    return true;
}

// Same as r_head but doesn't advance the cursor.
static bool r_head_peek(mb_cbor_r_t *r, uint8_t *out_major, uint64_t *out_arg) {
    size_t saved = r->pos;
    bool ok = r_head(r, out_major, out_arg);
    r->pos = saved;
    return ok;
}

mb_cbor_ty_t mb_cbor_r_peek(mb_cbor_r_t *r) {
    uint8_t major;
    uint64_t arg;
    if (!r_head_peek(r, &major, &arg)) return MB_CBOR_TY_INVALID;
    switch (major) {
        case 0: return MB_CBOR_TY_UINT;
        case 1: return MB_CBOR_TY_NINT;
        case 2: return MB_CBOR_TY_BSTR;
        case 3: return MB_CBOR_TY_TSTR;
        case 4: return MB_CBOR_TY_ARRAY;
        case 5: return MB_CBOR_TY_MAP;
        case 7:
            if (arg == 20 || arg == 21) return MB_CBOR_TY_BOOL;
            if (arg == 22) return MB_CBOR_TY_NULL;
            if (arg == 26 || arg == 27) return MB_CBOR_TY_FLOAT;
            return MB_CBOR_TY_INVALID;
        default:
            return MB_CBOR_TY_INVALID;
    }
}

bool mb_cbor_r_uint(mb_cbor_r_t *r, uint64_t *out) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 0) return r_fail(r, MB_EPROTO);
    *out = arg;
    return true;
}

bool mb_cbor_r_int(mb_cbor_r_t *r, int64_t *out) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major == 0) {
        if (arg > (uint64_t)INT64_MAX) return r_fail(r, MB_EPROTO);
        *out = (int64_t)arg;
        return true;
    }
    if (major == 1) {
        // -1 - arg. arg must fit so that -1 - arg >= INT64_MIN,
        // i.e. arg <= -1 - INT64_MIN == INT64_MAX (since INT64_MIN+1
        // negated is INT64_MAX).
        if (arg > (uint64_t)INT64_MAX) return r_fail(r, MB_EPROTO);
        *out = -1 - (int64_t)arg;
        return true;
    }
    return r_fail(r, MB_EPROTO);
}

bool mb_cbor_r_bool(mb_cbor_r_t *r, bool *out) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 7) return r_fail(r, MB_EPROTO);
    if (arg == 20) { *out = false; return true; }
    if (arg == 21) { *out = true;  return true; }
    return r_fail(r, MB_EPROTO);
}

bool mb_cbor_r_null(mb_cbor_r_t *r) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 7 || arg != 22) return r_fail(r, MB_EPROTO);
    return true;
}

bool mb_cbor_r_float(mb_cbor_r_t *r, double *out) {
    if (r->err) return false;
    if (r->pos >= r->len) return r_fail(r, MB_EPROTO);
    // Floats must be decoded from the head byte directly: r_head
    // swallows the info nibble, so the major-7 / info-26 / info-27
    // distinction between float32 and float64 would be lost.
    uint8_t b = r->buf[r->pos];
    if ((b >> 5) != 7) return r_fail(r, MB_EPROTO);
    uint8_t info = b & 0x1Fu;
    if (info == 26u) {
        if (r->pos + 5 > r->len) return r_fail(r, MB_EPROTO);
        uint32_t bits = ld_be32(r->buf + r->pos + 1);
        float f;
        memcpy(&f, &bits, sizeof(f));
        *out = (double)f;
        r->pos += 5;
        return true;
    }
    if (info == 27u) {
        if (r->pos + 9 > r->len) return r_fail(r, MB_EPROTO);
        uint64_t bits = ld_be64(r->buf + r->pos + 1);
        memcpy(out, &bits, sizeof(*out));
        r->pos += 9;
        return true;
    }
    return r_fail(r, MB_EPROTO);
}

bool mb_cbor_r_tstr(mb_cbor_r_t *r, const char **out_ptr, size_t *out_len) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 3) return r_fail(r, MB_EPROTO);
    if (arg > r->len - r->pos) return r_fail(r, MB_EPROTO);
    *out_ptr = (const char *)(r->buf + r->pos);
    *out_len = (size_t)arg;
    r->pos += (size_t)arg;
    return true;
}

bool mb_cbor_r_bstr(mb_cbor_r_t *r, const uint8_t **out_ptr, size_t *out_len) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 2) return r_fail(r, MB_EPROTO);
    if (arg > r->len - r->pos) return r_fail(r, MB_EPROTO);
    *out_ptr = r->buf + r->pos;
    *out_len = (size_t)arg;
    r->pos += (size_t)arg;
    return true;
}

bool mb_cbor_r_array_begin(mb_cbor_r_t *r, uint64_t *count) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 4) return r_fail(r, MB_EPROTO);
    *count = arg;
    return true;
}

bool mb_cbor_r_map_begin(mb_cbor_r_t *r, uint64_t *count) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    if (major != 5) return r_fail(r, MB_EPROTO);
    *count = arg;
    return true;
}

bool mb_cbor_r_skip(mb_cbor_r_t *r) {
    uint8_t major;
    uint64_t arg;
    if (!r_head(r, &major, &arg)) return false;
    switch (major) {
        case 0: case 1: case 7:
            // scalars: head is the whole item
            return true;
        case 2: case 3:
            if (arg > r->len - r->pos) return r_fail(r, MB_EPROTO);
            r->pos += (size_t)arg;
            return true;
        case 4:
            for (uint64_t i = 0; i < arg; i++) {
                if (!mb_cbor_r_skip(r)) return false;
            }
            return true;
        case 5:
            for (uint64_t i = 0; i < arg; i++) {
                if (!mb_cbor_r_skip(r)) return false;     // key
                if (!mb_cbor_r_skip(r)) return false;     // value
            }
            return true;
        case 6:
            // tagged: skip the enclosed item
            return mb_cbor_r_skip(r);
        default:
            return r_fail(r, MB_EPROTO);
    }
}
