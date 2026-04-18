// CopyCatOS — by Kyle Blizzard at Blizzard.show

// cbor.h — minimum CBOR encode/decode subset used by MoonBase IPC.
//
// IPC.md §2 locks the wire format to CBOR (RFC 8949) with integer
// keys in maps. Full CBOR is large; this file implements only what
// libmoonbase.so.1 and moonrock need to speak the protocol:
//   major 0  (unsigned int)
//   major 1  (negative int)
//   major 2  (byte string)        -- unused by current kinds, cheap
//   major 3  (text string, UTF-8)
//   major 4  (array, definite length)
//   major 5  (map, definite length, integer keys in our schemas)
//   major 7  (bool, null, float32/64 only)
//
// Anything out of that subset makes the decoder set MB_EPROTO.
//
// The writer grows a caller-owned buffer (or allocates one itself
// and transfers ownership on finish). The reader is a thin cursor
// over a borrowed buffer — readers do not copy strings or blobs;
// pointers returned from mb_cbor_r_tstr point inside the caller's
// buffer and are valid for as long as that buffer is.

#ifndef MOONBASE_IPC_CBOR_H
#define MOONBASE_IPC_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

typedef struct {
    uint8_t *buf;        // start of buffer
    size_t   cap;        // capacity in bytes
    size_t   len;        // bytes written so far
    bool     owns_buf;   // true -> free(buf) on drop
    int      err;        // 0 == ok, else an mb_error_t
} mb_cbor_w_t;

// Initialize with a caller-owned fixed buffer. Never reallocates;
// sets err = MB_ENOMEM on overflow.
void mb_cbor_w_init_fixed(mb_cbor_w_t *w, uint8_t *buf, size_t cap);

// Initialize with an internally-allocated, growable buffer.
// Finishing transfers ownership to the caller via mb_cbor_w_finish.
void mb_cbor_w_init_grow(mb_cbor_w_t *w, size_t initial_cap);

// Release any internally-owned buffer without handing it over.
void mb_cbor_w_drop(mb_cbor_w_t *w);

// Hand off the encoded buffer to the caller. After this call, *w is
// empty (cap == 0, len == 0, buf == NULL). Returns NULL if the
// writer ran into an error; in that case the buffer has already been
// freed. Call err() first to check.
uint8_t *mb_cbor_w_finish(mb_cbor_w_t *w, size_t *out_len);

bool mb_cbor_w_ok(const mb_cbor_w_t *w);
int  mb_cbor_w_err(const mb_cbor_w_t *w);

// Scalars.
void mb_cbor_w_uint(mb_cbor_w_t *w, uint64_t v);
void mb_cbor_w_int(mb_cbor_w_t *w, int64_t v);
void mb_cbor_w_bool(mb_cbor_w_t *w, bool v);
void mb_cbor_w_null(mb_cbor_w_t *w);
void mb_cbor_w_float(mb_cbor_w_t *w, double v);           // emits float32
void mb_cbor_w_float64(mb_cbor_w_t *w, double v);         // emits float64

// Text + byte strings. The _n variants take explicit length (no NUL
// terminator required); the NUL-string wrappers call strlen.
void mb_cbor_w_tstr(mb_cbor_w_t *w, const char *s);
void mb_cbor_w_tstr_n(mb_cbor_w_t *w, const char *s, size_t n);
void mb_cbor_w_bstr(mb_cbor_w_t *w, const uint8_t *b, size_t n);

// Containers. Begin writes the count; caller writes that many items.
void mb_cbor_w_array_begin(mb_cbor_w_t *w, size_t count);
void mb_cbor_w_map_begin(mb_cbor_w_t *w, size_t count);

// Sugar: map key is always a uint in our schemas.
static inline void mb_cbor_w_key(mb_cbor_w_t *w, uint64_t key) {
    mb_cbor_w_uint(w, key);
}

// ---------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------

typedef enum {
    MB_CBOR_TY_INVALID = 0,
    MB_CBOR_TY_UINT,
    MB_CBOR_TY_NINT,
    MB_CBOR_TY_BSTR,
    MB_CBOR_TY_TSTR,
    MB_CBOR_TY_ARRAY,
    MB_CBOR_TY_MAP,
    MB_CBOR_TY_BOOL,
    MB_CBOR_TY_NULL,
    MB_CBOR_TY_FLOAT,
} mb_cbor_ty_t;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    int            err;   // 0 == ok
} mb_cbor_r_t;

void mb_cbor_r_init(mb_cbor_r_t *r, const uint8_t *buf, size_t len);

bool mb_cbor_r_ok(const mb_cbor_r_t *r);
int  mb_cbor_r_err(const mb_cbor_r_t *r);
bool mb_cbor_r_at_end(const mb_cbor_r_t *r);

// Peek the next item's type without advancing the cursor. Returns
// MB_CBOR_TY_INVALID and sets err on malformed input.
mb_cbor_ty_t mb_cbor_r_peek(mb_cbor_r_t *r);

// Scalars. Return true on success; false on type mismatch or
// malformed input (with err set).
bool mb_cbor_r_uint(mb_cbor_r_t *r, uint64_t *out);
bool mb_cbor_r_int (mb_cbor_r_t *r, int64_t  *out);
bool mb_cbor_r_bool(mb_cbor_r_t *r, bool     *out);
bool mb_cbor_r_null(mb_cbor_r_t *r);
bool mb_cbor_r_float(mb_cbor_r_t *r, double  *out);

// Text string: the pointer returned points INSIDE the reader buffer
// and is valid for the buffer's lifetime. Not NUL-terminated; use
// *out_len. The caller may copy if it needs to outlive the buffer.
bool mb_cbor_r_tstr(mb_cbor_r_t *r, const char **out_ptr, size_t *out_len);
bool mb_cbor_r_bstr(mb_cbor_r_t *r, const uint8_t **out_ptr, size_t *out_len);

// Containers: *count receives the element/pair count. The caller
// reads that many items/pairs following.
bool mb_cbor_r_array_begin(mb_cbor_r_t *r, uint64_t *count);
bool mb_cbor_r_map_begin  (mb_cbor_r_t *r, uint64_t *count);

// Skip the next item, recursing into containers as needed. Used to
// tolerate unknown map keys (IPC.md §8: readers ignore unknown keys).
bool mb_cbor_r_skip(mb_cbor_r_t *r);

#ifdef __cplusplus
}
#endif

#endif // MOONBASE_IPC_CBOR_H
