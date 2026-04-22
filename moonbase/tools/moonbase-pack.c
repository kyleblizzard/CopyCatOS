// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-pack — assemble a `.appdev` directory into a shipping
// single-file `.app` bundle.
//
// Pipeline:
//   1. Validate the `.appdev` via mb_bundle_load (bundle-spec.md §8) so
//      we refuse to pack anything the launcher would reject at runtime.
//      mb_bundle_load also gives us the bundle-id from Info.appc —
//      required to write the trailer.
//   2. Locate the pre-built static ELF stub (--stub, then
//      $MOONBASE_APP_STUB, then the compile-time default).
//   3. mksquashfs the `.appdev` directory into a temporary .sqfs blob.
//      The whole bundle directory is packed so the squashfs root ends
//      up with `Contents/` as an immediate child — matches the layout
//      mb_bundle_load expects after the launcher mounts it.
//   4. Concatenate stub + sqfs into a sibling temp file, track the
//      squashfs offset and size.
//   5. Append the trailer via mb_appimg_write_trailer — same codepath
//      the unit + launcher tests already cover.
//   6. chmod 0755 (the stub is the ELF; the file needs to be
//      executable for a user to double-click) and rename into place.
//
// Usage:
//   moonbase-pack [--stub <path>] <src.appdev> [<dst.app>]
//
// Either `src` is .appdev and `dst` defaults to `<src-without-.appdev>.app`
// in the same directory, or both are given explicitly. --stub overrides
// the stub search chain for one-off development.
//
// Exit codes:
//   0   success
//   1   usage error
//   2   validation error (src not a valid bundle)
//   3   stub missing / unreadable
//   4   mksquashfs failed (not installed, or packing error)
//   5   I/O error during assembly

#include "bundle/appimg.h"
#include "bundle/bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

// MOONBASE_STUB_PATH is baked in by meson via -DMOONBASE_STUB_PATH=...
// It points at the installed stub under datadir. Developers and tests
// override via --stub or $MOONBASE_APP_STUB; the compile-time default
// is the fallback for a plain `moonbase-pack foo.appdev` invocation.
#ifndef MOONBASE_STUB_PATH
#define MOONBASE_STUB_PATH "/usr/local/share/moonbase/moonbase-app-stub"
#endif

static int usage(void) {
    fprintf(stderr,
        "usage: moonbase-pack [--stub <path>] <src.appdev> [<dst.app>]\n"
        "  Assembles src (a .appdev directory) into a single-file .app.\n"
        "  If dst is omitted, the output is placed alongside src with the\n"
        "  .appdev suffix replaced by .app.\n");
    return 1;
}

// Return 1 if `s` ends with `suffix`, else 0.
static int ends_with(const char *s, const char *suffix) {
    size_t ns = strlen(s), nsu = strlen(suffix);
    if (ns < nsu) return 0;
    return memcmp(s + ns - nsu, suffix, nsu) == 0;
}

// Strip a trailing slash if present (common from tab-completion on
// directories). Operates in-place on `s`.
static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') {
        s[n - 1] = '\0';
        n--;
    }
}

// Derive `<stem>.app` from `<stem>.appdev`. dst is caller-provided with
// at least `cap` bytes. Returns 0 on success, -1 if the derivation
// doesn't fit or `src` doesn't end in .appdev.
static int derive_dst_from_src(const char *src, char *dst, size_t cap) {
    size_t ns = strlen(src);
    const char *suf = ".appdev";
    size_t nsu = strlen(suf);
    if (ns < nsu || memcmp(src + ns - nsu, suf, nsu) != 0) return -1;
    size_t stem = ns - nsu;
    if (stem + 4 + 1 > cap) return -1;
    memcpy(dst, src, stem);
    memcpy(dst + stem, ".app", 5);          // includes trailing NUL
    return 0;
}

// Locate the stub binary. Lookup order: explicit --stub flag, then
// $MOONBASE_APP_STUB, then the compile-time default. Returns a
// pointer to the chosen path or NULL if none is readable. `err_buf`
// gets the reason on failure.
static const char *find_stub(const char *flag_stub,
                             char *err_buf, size_t err_cap) {
    const char *candidates[3];
    int n = 0;
    if (flag_stub) candidates[n++] = flag_stub;
    const char *env_stub = getenv("MOONBASE_APP_STUB");
    if (env_stub && env_stub[0]) candidates[n++] = env_stub;
    candidates[n++] = MOONBASE_STUB_PATH;

    for (int i = 0; i < n; i++) {
        if (access(candidates[i], R_OK) == 0) return candidates[i];
    }
    snprintf(err_buf, err_cap,
        "no stub readable; tried: %s%s%s%s%s",
        flag_stub ? flag_stub : "",
        flag_stub ? ", " : "",
        env_stub ? env_stub : "",
        env_stub ? ", " : "",
        MOONBASE_STUB_PATH);
    return NULL;
}

// Run mksquashfs on `src` (a directory) → `dst` (output path). Uses the
// same settings bundle-spec.md calls for: zstd compression, 128 KiB
// block size, -noappend. Progress output is redirected to /dev/null;
// errors still reach stderr so a failure surfaces cleanly.
static int run_mksquashfs(const char *src, const char *dst) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
        char *args[] = {
            (char *)"mksquashfs", (char *)src, (char *)dst,
            (char *)"-comp", (char *)"zstd",
            (char *)"-b", (char *)"131072",
            (char *)"-noappend",
            NULL,
        };
        execvp("mksquashfs", args);
        // execvp only returns on failure.
        fprintf(stderr, "moonbase-pack: exec mksquashfs: %s\n",
                strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Copy the contents of `src` onto an already-open `dst_fd` at its
// current position. Returns total bytes copied or -1 on error.
static ssize_t concat_file(int dst_fd, const char *src) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    uint8_t buf[64 * 1024];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in);
            return -1;
        }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(in);
                return -1;
            }
            off += w;
        }
        total += n;
    }
    close(in);
    return total;
}

int main(int argc, char **argv) {
    const char *flag_stub = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[i], "--stub") == 0) {
            if (i + 1 >= argc) return usage();
            flag_stub = argv[i + 1];
            i += 2;
            continue;
        }
        fprintf(stderr, "moonbase-pack: unknown flag: %s\n", argv[i]);
        return usage();
    }

    if (argc - i < 1 || argc - i > 2) return usage();

    // Normalise src: drop trailing slash so ends_with(".appdev") works
    // on both `foo.appdev` and `foo.appdev/`.
    char src[PATH_MAX];
    if (snprintf(src, sizeof(src), "%s", argv[i]) >= (int)sizeof(src)) {
        fprintf(stderr, "moonbase-pack: src path too long\n");
        return 1;
    }
    strip_trailing_slash(src);

    if (!ends_with(src, ".appdev")) {
        fprintf(stderr,
            "moonbase-pack: src must end in .appdev (got %s)\n", src);
        return 1;
    }

    char dst[PATH_MAX];
    if (argc - i == 2) {
        if (snprintf(dst, sizeof(dst), "%s", argv[i + 1]) >= (int)sizeof(dst)) {
            fprintf(stderr, "moonbase-pack: dst path too long\n");
            return 1;
        }
        strip_trailing_slash(dst);
        if (!ends_with(dst, ".app")) {
            fprintf(stderr,
                "moonbase-pack: dst must end in .app (got %s)\n", dst);
            return 1;
        }
    } else {
        if (derive_dst_from_src(src, dst, sizeof(dst)) != 0) {
            fprintf(stderr, "moonbase-pack: can't derive dst from src\n");
            return 1;
        }
    }

    // Step 1: validate the source bundle.
    char err[512];
    mb_bundle_t bundle;
    mb_bundle_err_t be = mb_bundle_load(src, &bundle, err, sizeof(err));
    if (be != MB_BUNDLE_OK) {
        fprintf(stderr, "moonbase-pack: %s: %s (%s)\n",
                src, mb_bundle_err_string(be), err);
        return 2;
    }
    const char *bundle_id = bundle.info.id;
    if (!bundle_id || !bundle_id[0]) {
        fprintf(stderr, "moonbase-pack: bundle.id missing from Info.appc\n");
        mb_bundle_free(&bundle);
        return 2;
    }

    // Step 2: locate the stub.
    const char *stub_path = find_stub(flag_stub, err, sizeof(err));
    if (!stub_path) {
        fprintf(stderr, "moonbase-pack: %s\n", err);
        mb_bundle_free(&bundle);
        return 3;
    }

    // Step 3: mksquashfs the source into a temp sibling of `dst`. Same
    // directory as dst so a final rename(2) stays atomic on the same
    // filesystem.
    char dst_dir[PATH_MAX];
    if (snprintf(dst_dir, sizeof(dst_dir), "%s", dst) >= (int)sizeof(dst_dir)) {
        fprintf(stderr, "moonbase-pack: dst path too long\n");
        mb_bundle_free(&bundle);
        return 1;
    }
    char *dd = dirname(dst_dir);

    char sqfs_tmp[PATH_MAX];
    if (snprintf(sqfs_tmp, sizeof(sqfs_tmp), "%s/.moonbase-pack-XXXXXX.sqfs",
                 dd) >= (int)sizeof(sqfs_tmp)) {
        fprintf(stderr, "moonbase-pack: tmp sqfs path too long\n");
        mb_bundle_free(&bundle);
        return 1;
    }
    // mkstemps with suffix ".sqfs" (length 5): pattern must end in
    // XXXXXX + the suffix.
    int sqfs_fd = mkstemps(sqfs_tmp, 5);
    if (sqfs_fd < 0) {
        fprintf(stderr, "moonbase-pack: mkstemps(%s): %s\n",
                sqfs_tmp, strerror(errno));
        mb_bundle_free(&bundle);
        return 5;
    }
    close(sqfs_fd);
    // mksquashfs wants to create the file itself — unlink so it doesn't
    // see a zero-byte file and bail. The parent-dir permissions still
    // gate who can create it.
    unlink(sqfs_tmp);

    int mk_rc = run_mksquashfs(src, sqfs_tmp);
    if (mk_rc != 0) {
        fprintf(stderr,
            "moonbase-pack: mksquashfs failed (exit %d); is mksquashfs "
            "installed? (squashfs-tools)\n", mk_rc);
        unlink(sqfs_tmp);
        mb_bundle_free(&bundle);
        return 4;
    }

    struct stat sqfs_st;
    if (stat(sqfs_tmp, &sqfs_st) != 0) {
        fprintf(stderr, "moonbase-pack: stat sqfs: %s\n", strerror(errno));
        unlink(sqfs_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    // Step 4: assemble stub + sqfs into a temp .app sibling.
    char app_tmp[PATH_MAX];
    if (snprintf(app_tmp, sizeof(app_tmp), "%s/.moonbase-pack-XXXXXX.app",
                 dd) >= (int)sizeof(app_tmp)) {
        fprintf(stderr, "moonbase-pack: tmp app path too long\n");
        unlink(sqfs_tmp);
        mb_bundle_free(&bundle);
        return 1;
    }
    int app_fd = mkstemps(app_tmp, 4);   // suffix ".app" (4 chars)
    if (app_fd < 0) {
        fprintf(stderr, "moonbase-pack: mkstemps(%s): %s\n",
                app_tmp, strerror(errno));
        unlink(sqfs_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    ssize_t stub_bytes = concat_file(app_fd, stub_path);
    if (stub_bytes < 0) {
        fprintf(stderr, "moonbase-pack: copy stub %s: %s\n",
                stub_path, strerror(errno));
        close(app_fd);
        unlink(sqfs_tmp);
        unlink(app_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    ssize_t sqfs_bytes = concat_file(app_fd, sqfs_tmp);
    if (sqfs_bytes < 0 || sqfs_bytes != (ssize_t)sqfs_st.st_size) {
        fprintf(stderr, "moonbase-pack: copy sqfs: %s\n", strerror(errno));
        close(app_fd);
        unlink(sqfs_tmp);
        unlink(app_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    // Step 5: trailer.
    mb_appimg_err_t ae = mb_appimg_write_trailer(
        app_fd,
        (uint64_t)stub_bytes,
        (uint64_t)sqfs_bytes,
        bundle_id,
        err, sizeof(err));
    if (ae != MB_APPIMG_OK) {
        fprintf(stderr, "moonbase-pack: write trailer: %s (%s)\n",
                mb_appimg_err_string(ae), err);
        close(app_fd);
        unlink(sqfs_tmp);
        unlink(app_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    // Step 6: chmod 0755 + rename into place. fchmod on the fd before
    // rename means the file lands with the right mode atomically (no
    // window where a reader sees a 0600 .app).
    if (fchmod(app_fd, 0755) != 0) {
        fprintf(stderr, "moonbase-pack: fchmod: %s\n", strerror(errno));
        close(app_fd);
        unlink(sqfs_tmp);
        unlink(app_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }
    close(app_fd);

    unlink(sqfs_tmp);

    // rename(2) overwrites dst atomically on the same filesystem. If
    // dst already exists and is a symlink (the meson build leaves
    // foo.app → foo.appdev in the build tree until this slice takes
    // over), unlink it first so rename doesn't follow it.
    struct stat dst_st;
    if (lstat(dst, &dst_st) == 0 && S_ISLNK(dst_st.st_mode)) {
        unlink(dst);
    }
    if (rename(app_tmp, dst) != 0) {
        fprintf(stderr, "moonbase-pack: rename %s -> %s: %s\n",
                app_tmp, dst, strerror(errno));
        unlink(app_tmp);
        mb_bundle_free(&bundle);
        return 5;
    }

    // Step 7: icon-cache xattr (bundle-spec §1.4). Transport-only — we
    // copy the bundle's AppIcon.png bytes onto the shipping .app file as
    // user.moonbase.icon-cache so fileviewer/dock/searchsystem can draw
    // an icon without mounting the squashfs. The developer is
    // responsible for shipping a correctly-sized 128x128 PNG; we size-
    // gate at 16 KiB and warn-and-skip otherwise. Missing icon → silent
    // skip (fallback is a generic app icon in fileviewer).
    char icon_src[PATH_MAX];
    if (snprintf(icon_src, sizeof(icon_src),
                 "%s/Contents/Resources/AppIcon.png", src)
            < (int)sizeof(icon_src)) {
        struct stat icon_st;
        if (stat(icon_src, &icon_st) == 0 && S_ISREG(icon_st.st_mode)) {
            if (icon_st.st_size > 16 * 1024) {
                fprintf(stderr,
                    "moonbase-pack: %s is %lld bytes (>16 KiB cap); "
                    "icon-cache xattr skipped\n",
                    icon_src, (long long)icon_st.st_size);
            } else {
                int ifd = open(icon_src, O_RDONLY | O_CLOEXEC);
                if (ifd >= 0) {
                    uint8_t ibuf[16 * 1024];
                    ssize_t in = 0;
                    while (in < (ssize_t)sizeof(ibuf)) {
                        ssize_t r = read(ifd, ibuf + in,
                                         sizeof(ibuf) - (size_t)in);
                        if (r < 0) { if (errno == EINTR) continue; break; }
                        if (r == 0) break;
                        in += r;
                    }
                    close(ifd);
                    if (in > 0) {
                        if (setxattr(dst, "user.moonbase.icon-cache",
                                     ibuf, (size_t)in, 0) != 0) {
                            // ENOTSUP/EOPNOTSUPP: filesystem doesn't do
                            // user xattrs (tmpfs without user_xattr,
                            // some FUSE mounts). Silent — not a pack
                            // failure; fileviewer falls back to a
                            // generic icon.
                            if (errno != ENOTSUP
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
                                && errno != EOPNOTSUPP
#endif
                            ) {
                                fprintf(stderr,
                                    "moonbase-pack: setxattr icon-cache "
                                    "on %s: %s\n",
                                    dst, strerror(errno));
                            }
                        }
                    }
                }
            }
        }
    }

    struct stat final_st;
    if (stat(dst, &final_st) == 0) {
        printf("packed: %s (%lld bytes, bundle-id %s)\n",
               dst, (long long)final_st.st_size, bundle_id);
    }

    mb_bundle_free(&bundle);
    return 0;
}
