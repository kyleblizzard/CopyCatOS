// CopyCatOS — by Kyle Blizzard at Blizzard.show

// host_util.c — implementation of the small helpers in host_util.h.
// Compositor-agnostic; relies only on POSIX + libc.

#include "host_util.h"
#include "host_chrome.h"  // MB_CHROME_TITLEBAR_HEIGHT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

char *mb_host_default_socket_path(void)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) return NULL;
    size_t len = strlen(xdg) + strlen("/moonbase.sock") + 1;
    char *p = malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/moonbase.sock", xdg);
    return p;
}

uint64_t mb_host_ts_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

long mb_host_rect_intersection_area(int ax, int ay, int aw, int ah,
                                    int bx, int by, int bw, int bh)
{
    int x0 = ax > bx ? ax : bx;
    int y0 = ay > by ? ay : by;
    int x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    int y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    if (x1 <= x0 || y1 <= y0) return 0;
    return (long)(x1 - x0) * (long)(y1 - y0);
}

void mb_host_chrome_px_from_points(int points_w, int points_h, float scale,
                                   uint32_t *out_w, uint32_t *out_h)
{
    int content_w_px = (int)(points_w * scale + 0.5f);
    int content_h_px = (int)(points_h * scale + 0.5f);
    int titlebar_px  = (int)(MB_CHROME_TITLEBAR_HEIGHT * scale + 0.5f);
    if (out_w) *out_w = (uint32_t)content_w_px;
    if (out_h) *out_h = (uint32_t)(content_h_px + titlebar_px);
}

bool mb_host_validate_and_map_commit_fd(uint32_t stride, uint32_t height,
                                        int fd,
                                        void **out_map, size_t *out_size)
{
    if (!out_map || !out_size) return false;
    *out_map = NULL;
    *out_size = 0;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr,
                "[mb_host] WINDOW_COMMIT: fstat failed on fd %d\n", fd);
        return false;
    }

    size_t need = (size_t)stride * (size_t)height;
    if ((size_t)st.st_size < need) {
        fprintf(stderr,
                "[mb_host] WINDOW_COMMIT: buffer too small (%zu bytes, "
                "need %zu)\n", (size_t)st.st_size, need);
        return false;
    }

    // MAP_SHARED so we see any future writes the client makes — in
    // practice the client does one draw and unmaps, so the buffer
    // contents are stable by the time we upload, but MAP_SHARED is the
    // semantically correct mode for a buffer whose ownership is about
    // to be shared.
    void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr,
                "[mb_host] WINDOW_COMMIT: mmap failed on fd %d\n", fd);
        return false;
    }

    *out_map = p;
    *out_size = need;
    return true;
}
