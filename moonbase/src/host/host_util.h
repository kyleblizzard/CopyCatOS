// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_util.h — small compositor-agnostic helpers shared by moonrock
// and moonrock-lite. Path resolution, monotonic timestamps, geometry
// math, and the WINDOW_COMMIT fd validate-and-mmap helper.

#ifndef MB_HOST_UTIL_H
#define MB_HOST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocate "$XDG_RUNTIME_DIR/moonbase.sock". Caller frees. Returns NULL
// when XDG_RUNTIME_DIR is unset/empty or memory is exhausted.
char *mb_host_default_socket_path(void);

// Monotonic microseconds since boot. Used to stamp outgoing input,
// drag, and window-event frames. Matches mb_event_t.timestamp_us
// semantics on the client side.
uint64_t mb_host_ts_us(void);

// Axis-aligned rectangle intersection area. Returns 0 when the rects
// are disjoint. Used to pick a surface's host output for Per-Monitor
// DPI v2 scale migration.
long mb_host_rect_intersection_area(int ax, int ay, int aw, int ah,
                                    int bx, int by, int bw, int bh);

// Compute chrome physical-pixel footprint from the client-declared
// points size at the given backing scale. Mirrors host_chrome's layout:
// chrome has no side or bottom inset, so chrome_w == content_w_px and
// chrome_h == content_h_px + titlebar_px.
void mb_host_chrome_px_from_points(int points_w, int points_h, float scale,
                                   uint32_t *out_w, uint32_t *out_h);

// fstat the SCM_RIGHTS'd commit fd, validate it can hold stride*height
// bytes, mmap PROT_READ/MAP_SHARED. On success: *out_map = mmap pointer,
// *out_size = mapping size in bytes, returns true. On failure logs to
// stderr and returns false (consumers free no resources — the fd is
// owned by the IPC layer and closed after the callback returns).
bool mb_host_validate_and_map_commit_fd(uint32_t stride, uint32_t height,
                                        int fd,
                                        void **out_map, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif // MB_HOST_UTIL_H
