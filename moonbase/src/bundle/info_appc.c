// CopyCatOS — by Kyle Blizzard at Blizzard.show

// info_appc — schema-aware loader. Thin layer over toml_lite that
// checks everything moonbase-launch has to trust before it execs a
// bundle: required tables, required keys, enumerated values, and the
// reverse-DNS bundle-id form.

#include "info_appc.h"
#include "toml_lite.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// -------------------------------------------------------------------
// string helpers
// -------------------------------------------------------------------

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int in_list(const char *s, const char *const *list) {
    for (; *list; list++) if (strcmp(s, *list) == 0) return 1;
    return 0;
}

static void set_err(char *buf, size_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

// -------------------------------------------------------------------
// allowlists — pulled straight from info_appc_schema.md
// -------------------------------------------------------------------

static const char *const LANG_VALUES[]     = { "c", "web", "python", "rust", "swift", NULL };
static const char *const RENDER_VALUES[]   = { "cairo", "gl", NULL };
static const char *const CATEGORY_VALUES[] = {
    "utility", "productivity", "developer", "graphics", "media",
    "education", "games", "system", "other", NULL,
};
static const char *const CHANNEL_VALUES[]  = { "stable", "beta", NULL };
static const char *const WRAP_TOOLKIT_VALUES[] = {
    "native", "qt5", "qt6", "gtk3", "gtk4", NULL,
};

static const char *const PERM_FS_VALUES[] = {
    "documents:read", "documents:read-write",
    "downloads:read", "downloads:read-write",
    "desktop:read",   "desktop:read-write",
    "music:read", "pictures:read", "movies:read",
    "user-chosen", NULL,
};
static const char *const PERM_NET_VALUES[] = {
    "outbound:http", "outbound:https",
    "outbound:ws",   "outbound:wss",
    "inbound:localhost", NULL,
};
static const char *const PERM_HW_VALUES[] = {
    "camera", "microphone", "location", "bluetooth",
    "controller:read", "controller:rumble", NULL,
};
static const char *const PERM_SYS_VALUES[] = {
    "notifications:post", "notifications:receive-all",
    "clipboard-monitor", "accessibility", "keychain", "printing",
    "process-list", NULL,
};

// Reverse-DNS check — matches `^[a-z][a-z0-9.-]*\.[a-z0-9.-]+$` from the
// schema doc. Same rule is reused for [permissions].ipc entries.
static int is_reverse_dns(const char *s) {
    if (!s || !*s) return 0;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return 0;
    int saw_dot = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9')
              || c == '.' || c == '-';
        if (!ok) return 0;
        if (c == '.') saw_dot = 1;
    }
    if (!saw_dot) return 0;
    if (s[strlen(s) - 1] == '.' || s[strlen(s) - 1] == '-') return 0;
    return 1;
}

// -------------------------------------------------------------------
// value helpers — wrap toml_lite getters with required/optional semantics
// -------------------------------------------------------------------

static mb_info_appc_err_t need_string(const tl_table *t, const char *key,
                                      const char *tbl_name,
                                      char **out, char *err, size_t err_cap) {
    const char *s = tl_table_string(t, key);
    if (!s) {
        set_err(err, err_cap, "[%s].%s is required and must be a string",
                tbl_name, key);
        return MB_INFO_APPC_ERR_MISSING_REQUIRED;
    }
    *out = str_dup(s);
    if (!*out) { set_err(err, err_cap, "out of memory"); return MB_INFO_APPC_ERR_NO_MEM; }
    return MB_INFO_APPC_OK;
}

// Optional string: if present, duplicated; if absent, *out left unchanged (NULL).
static mb_info_appc_err_t opt_string(const tl_table *t, const char *key,
                                     char **out, char *err, size_t err_cap) {
    const char *s = tl_table_string(t, key);
    if (!s) return MB_INFO_APPC_OK;
    *out = str_dup(s);
    if (!*out) { set_err(err, err_cap, "out of memory"); return MB_INFO_APPC_ERR_NO_MEM; }
    return MB_INFO_APPC_OK;
}

// Validate string against allowlist. Returns OK if absent (optional). If
// present and not in list, error.
static mb_info_appc_err_t opt_enum(const tl_table *t, const char *key,
                                   const char *tbl_name,
                                   const char *const *allowed,
                                   char **out, char *err, size_t err_cap) {
    const char *s = tl_table_string(t, key);
    if (!s) return MB_INFO_APPC_OK;
    if (!in_list(s, allowed)) {
        set_err(err, err_cap, "[%s].%s = \"%s\" not in allowlist",
                tbl_name, key, s);
        return MB_INFO_APPC_ERR_UNKNOWN_VALUE;
    }
    *out = str_dup(s);
    if (!*out) { set_err(err, err_cap, "out of memory"); return MB_INFO_APPC_ERR_NO_MEM; }
    return MB_INFO_APPC_OK;
}

// Deep-copy a string-array from a table, validating each element against
// an allowlist (or against is_reverse_dns for ipc). Returns OK if key
// absent (callers supply the required-vs-optional policy).
static mb_info_appc_err_t copy_string_array(const tl_table *t,
                                            const char *key,
                                            const char *tbl_name,
                                            const char *const *allowed,
                                            int rdns_mode,
                                            char ***out, size_t *out_count,
                                            char *err, size_t err_cap) {
    const char *const *items = NULL;
    size_t count = tl_table_string_array(t, key, &items);
    if (count == 0) return MB_INFO_APPC_OK;

    char **arr = calloc(count + 1, sizeof(char *));
    if (!arr) { set_err(err, err_cap, "out of memory"); return MB_INFO_APPC_ERR_NO_MEM; }

    for (size_t i = 0; i < count; i++) {
        if (rdns_mode) {
            if (!is_reverse_dns(items[i])) {
                for (size_t j = 0; j < i; j++) free(arr[j]);
                free(arr);
                set_err(err, err_cap, "[%s].%s[%zu] = \"%s\" must be reverse-DNS",
                        tbl_name, key, i, items[i]);
                return MB_INFO_APPC_ERR_BAD_VALUE;
            }
        } else if (allowed) {
            if (!in_list(items[i], allowed)) {
                for (size_t j = 0; j < i; j++) free(arr[j]);
                free(arr);
                set_err(err, err_cap,
                        "[%s].%s[%zu] = \"%s\" not in allowlist",
                        tbl_name, key, i, items[i]);
                return MB_INFO_APPC_ERR_UNKNOWN_VALUE;
            }
        }
        arr[i] = str_dup(items[i]);
        if (!arr[i]) {
            for (size_t j = 0; j < i; j++) free(arr[j]);
            free(arr);
            set_err(err, err_cap, "out of memory");
            return MB_INFO_APPC_ERR_NO_MEM;
        }
    }
    *out = arr;
    *out_count = count;
    return MB_INFO_APPC_OK;
}

static mb_info_appc_err_t parse_lang(const char *s, mb_info_appc_lang_t *out) {
    if (strcmp(s, "c")      == 0) { *out = MB_INFO_APPC_LANG_C;      return MB_INFO_APPC_OK; }
    if (strcmp(s, "web")    == 0) { *out = MB_INFO_APPC_LANG_WEB;    return MB_INFO_APPC_OK; }
    if (strcmp(s, "python") == 0) { *out = MB_INFO_APPC_LANG_PYTHON; return MB_INFO_APPC_OK; }
    if (strcmp(s, "rust")   == 0) { *out = MB_INFO_APPC_LANG_RUST;   return MB_INFO_APPC_OK; }
    if (strcmp(s, "swift")  == 0) { *out = MB_INFO_APPC_LANG_SWIFT;  return MB_INFO_APPC_OK; }
    return MB_INFO_APPC_ERR_UNKNOWN_VALUE;
}

static mb_info_appc_err_t parse_render(const char *s, mb_info_appc_render_t *out) {
    if (strcmp(s, "cairo") == 0) { *out = MB_INFO_APPC_RENDER_CAIRO; return MB_INFO_APPC_OK; }
    if (strcmp(s, "gl")    == 0) { *out = MB_INFO_APPC_RENDER_GL;    return MB_INFO_APPC_OK; }
    return MB_INFO_APPC_ERR_UNKNOWN_VALUE;
}

static mb_info_appc_err_t parse_wrap_toolkit(const char *s,
                                             mb_info_appc_wrap_toolkit_t *out) {
    if (strcmp(s, "native") == 0) { *out = MB_INFO_APPC_WRAP_NATIVE; return MB_INFO_APPC_OK; }
    if (strcmp(s, "qt5")    == 0) { *out = MB_INFO_APPC_WRAP_QT5;    return MB_INFO_APPC_OK; }
    if (strcmp(s, "qt6")    == 0) { *out = MB_INFO_APPC_WRAP_QT6;    return MB_INFO_APPC_OK; }
    if (strcmp(s, "gtk3")   == 0) { *out = MB_INFO_APPC_WRAP_GTK3;   return MB_INFO_APPC_OK; }
    if (strcmp(s, "gtk4")   == 0) { *out = MB_INFO_APPC_WRAP_GTK4;   return MB_INFO_APPC_OK; }
    return MB_INFO_APPC_ERR_UNKNOWN_VALUE;
}

// -------------------------------------------------------------------
// primary parse
// -------------------------------------------------------------------

mb_info_appc_err_t mb_info_appc_parse_buffer(const char *src, size_t len,
                                             mb_info_appc_t *out,
                                             char *err_buf, size_t err_cap) {
    if (!out) return MB_INFO_APPC_ERR_BAD_VALUE;
    memset(out, 0, sizeof(*out));

    if (len > MB_INFO_APPC_MAX_BYTES) {
        set_err(err_buf, err_cap, "Info.appc exceeds %u-byte cap", MB_INFO_APPC_MAX_BYTES);
        return MB_INFO_APPC_ERR_TOO_LARGE;
    }

    tl_doc *doc = NULL;
    char terr[192];
    tl_error te = tl_parse(src, len, &doc, terr, sizeof(terr));
    if (te != TL_OK) {
        set_err(err_buf, err_cap, "TOML parse: %s", terr);
        return MB_INFO_APPC_ERR_PARSE;
    }

    mb_info_appc_err_t rc = MB_INFO_APPC_OK;

    // ---- [bundle] (required) --------------------------------------
    const tl_table *bundle = tl_doc_get_table(doc, "bundle");
    if (!bundle) {
        set_err(err_buf, err_cap, "[bundle] table is required");
        rc = MB_INFO_APPC_ERR_MISSING_REQUIRED; goto done;
    }
    if ((rc = need_string(bundle, "id", "bundle", &out->id, err_buf, err_cap))) goto done;
    if (!is_reverse_dns(out->id)) {
        set_err(err_buf, err_cap,
                "[bundle].id \"%s\" must match reverse-DNS form "
                "^[a-z][a-z0-9.-]*\\.[a-z0-9.-]+$", out->id);
        rc = MB_INFO_APPC_ERR_BAD_VALUE; goto done;
    }
    if ((rc = need_string(bundle, "name", "bundle", &out->name, err_buf, err_cap))) goto done;
    if ((rc = need_string(bundle, "version", "bundle", &out->version, err_buf, err_cap))) goto done;
    if ((rc = need_string(bundle, "minimum-moonbase", "bundle",
                          &out->minimum_moonbase, err_buf, err_cap))) goto done;
    if ((rc = opt_string(bundle, "copyright", &out->copyright, err_buf, err_cap))) goto done;
    if ((rc = opt_enum(bundle, "category", "bundle", CATEGORY_VALUES,
                       &out->category, err_buf, err_cap))) goto done;

    // ---- [executable] (required) ----------------------------------
    const tl_table *exec = tl_doc_get_table(doc, "executable");
    if (!exec) {
        set_err(err_buf, err_cap, "[executable] table is required");
        rc = MB_INFO_APPC_ERR_MISSING_REQUIRED; goto done;
    }
    if ((rc = need_string(exec, "path", "executable", &out->exec_path, err_buf, err_cap))) goto done;
    char *lang_str = NULL;
    if ((rc = need_string(exec, "language", "executable", &lang_str, err_buf, err_cap))) goto done;
    if (!in_list(lang_str, LANG_VALUES)) {
        set_err(err_buf, err_cap, "[executable].language = \"%s\" not in allowlist", lang_str);
        free(lang_str);
        rc = MB_INFO_APPC_ERR_UNKNOWN_VALUE; goto done;
    }
    parse_lang(lang_str, &out->lang);
    free(lang_str);

    out->render_default = MB_INFO_APPC_RENDER_DEFAULT;
    const char *rm = tl_table_string(exec, "render-mode-default");
    if (rm) {
        if (!in_list(rm, RENDER_VALUES)) {
            set_err(err_buf, err_cap,
                    "[executable].render-mode-default = \"%s\" not in allowlist", rm);
            rc = MB_INFO_APPC_ERR_UNKNOWN_VALUE; goto done;
        }
        parse_render(rm, &out->render_default);
    }

    // ---- [permissions] (required, may be empty) --------------------
    const tl_table *perms = tl_doc_get_table(doc, "permissions");
    if (!perms) {
        set_err(err_buf, err_cap, "[permissions] table is required (may be empty)");
        rc = MB_INFO_APPC_ERR_MISSING_REQUIRED; goto done;
    }
    if ((rc = copy_string_array(perms, "filesystem", "permissions",
                                PERM_FS_VALUES, 0,
                                &out->perm_filesystem, &out->perm_filesystem_count,
                                err_buf, err_cap))) goto done;
    if ((rc = copy_string_array(perms, "network", "permissions",
                                PERM_NET_VALUES, 0,
                                &out->perm_network, &out->perm_network_count,
                                err_buf, err_cap))) goto done;
    if ((rc = copy_string_array(perms, "hardware", "permissions",
                                PERM_HW_VALUES, 0,
                                &out->perm_hardware, &out->perm_hardware_count,
                                err_buf, err_cap))) goto done;
    if ((rc = copy_string_array(perms, "system", "permissions",
                                PERM_SYS_VALUES, 0,
                                &out->perm_system, &out->perm_system_count,
                                err_buf, err_cap))) goto done;
    if ((rc = copy_string_array(perms, "ipc", "permissions",
                                NULL, 1,
                                &out->perm_ipc, &out->perm_ipc_count,
                                err_buf, err_cap))) goto done;

    // ---- [web] (required iff lang == web) --------------------------
    const tl_table *web = tl_doc_get_table(doc, "web");
    if (out->lang == MB_INFO_APPC_LANG_WEB) {
        if (!web) {
            set_err(err_buf, err_cap,
                    "[web] table is required when [executable].language = \"web\"");
            rc = MB_INFO_APPC_ERR_MISSING_REQUIRED; goto done;
        }
        if ((rc = need_string(web, "main-url", "web", &out->web_main_url, err_buf, err_cap))) goto done;
        if ((rc = opt_string(web, "manifest-url", &out->web_manifest_url, err_buf, err_cap))) goto done;
        if ((rc = copy_string_array(web, "allowed-origins", "web",
                                    NULL, 0,
                                    &out->web_allowed_origins,
                                    &out->web_allowed_origins_count,
                                    err_buf, err_cap))) goto done;
    }

    // ---- [localization] (optional) --------------------------------
    const tl_table *loc = tl_doc_get_table(doc, "localization");
    if (loc) {
        if ((rc = opt_string(loc, "base-locale", &out->base_locale, err_buf, err_cap))) goto done;
        if ((rc = copy_string_array(loc, "supported-locales", "localization",
                                    NULL, 0,
                                    &out->supported_locales,
                                    &out->supported_locales_count,
                                    err_buf, err_cap))) goto done;
    }

    // ---- [update] (optional) --------------------------------------
    const tl_table *upd = tl_doc_get_table(doc, "update");
    if (upd) {
        if ((rc = opt_string(upd, "url", &out->update_url, err_buf, err_cap))) goto done;
        if ((rc = opt_enum(upd, "channel", "update", CHANNEL_VALUES,
                           &out->update_channel, err_buf, err_cap))) goto done;
    }

    // ---- [wrap] (optional) ----------------------------------------
    out->wrap_toolkit = MB_INFO_APPC_WRAP_NATIVE;
    const tl_table *wrap = tl_doc_get_table(doc, "wrap");
    if (wrap) {
        const char *tk = tl_table_string(wrap, "toolkit");
        if (tk) {
            if (!in_list(tk, WRAP_TOOLKIT_VALUES)) {
                set_err(err_buf, err_cap,
                        "[wrap].toolkit = \"%s\" not in allowlist", tk);
                rc = MB_INFO_APPC_ERR_UNKNOWN_VALUE; goto done;
            }
            parse_wrap_toolkit(tk, &out->wrap_toolkit);
        }
    }

done:
    tl_doc_free(doc);
    if (rc != MB_INFO_APPC_OK) {
        mb_info_appc_free(out);
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

mb_info_appc_err_t mb_info_appc_parse_file(const char *path,
                                           mb_info_appc_t *out,
                                           char *err_buf, size_t err_cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err_buf, err_cap, "open(%s): %s", path, strerror(errno));
        return MB_INFO_APPC_ERR_PARSE;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_err(err_buf, err_cap, "fstat(%s): %s", path, strerror(errno));
        close(fd);
        return MB_INFO_APPC_ERR_PARSE;
    }
    if ((size_t)st.st_size > MB_INFO_APPC_MAX_BYTES) {
        set_err(err_buf, err_cap, "%s exceeds %u-byte cap", path, MB_INFO_APPC_MAX_BYTES);
        close(fd);
        return MB_INFO_APPC_ERR_TOO_LARGE;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        set_err(err_buf, err_cap, "out of memory");
        close(fd);
        return MB_INFO_APPC_ERR_NO_MEM;
    }
    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + total, (size_t)st.st_size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(err_buf, err_cap, "read(%s): %s", path, strerror(errno));
            free(buf); close(fd);
            return MB_INFO_APPC_ERR_PARSE;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    mb_info_appc_err_t rc = mb_info_appc_parse_buffer(buf, total, out, err_buf, err_cap);
    free(buf);
    return rc;
}

// -------------------------------------------------------------------
// free + error strings
// -------------------------------------------------------------------

static void free_str_array(char **arr, size_t count) {
    if (!arr) return;
    for (size_t i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

void mb_info_appc_free(mb_info_appc_t *info) {
    if (!info) return;
    free(info->id); free(info->name); free(info->version);
    free(info->minimum_moonbase); free(info->copyright); free(info->category);
    free(info->exec_path);
    free_str_array(info->perm_filesystem, info->perm_filesystem_count);
    free_str_array(info->perm_network,    info->perm_network_count);
    free_str_array(info->perm_hardware,   info->perm_hardware_count);
    free_str_array(info->perm_system,     info->perm_system_count);
    free_str_array(info->perm_ipc,        info->perm_ipc_count);
    free(info->web_main_url); free(info->web_manifest_url);
    free_str_array(info->web_allowed_origins, info->web_allowed_origins_count);
    free(info->base_locale);
    free_str_array(info->supported_locales, info->supported_locales_count);
    free(info->update_url); free(info->update_channel);
    memset(info, 0, sizeof(*info));
}

const char *mb_info_appc_err_string(mb_info_appc_err_t e) {
    switch (e) {
        case MB_INFO_APPC_OK:                  return "ok";
        case MB_INFO_APPC_ERR_TOO_LARGE:       return "too-large";
        case MB_INFO_APPC_ERR_PARSE:           return "parse-error";
        case MB_INFO_APPC_ERR_MISSING_REQUIRED:return "missing-required";
        case MB_INFO_APPC_ERR_BAD_VALUE:       return "bad-value";
        case MB_INFO_APPC_ERR_UNKNOWN_VALUE:   return "unknown-value";
        case MB_INFO_APPC_ERR_NO_MEM:          return "no-mem";
    }
    return "unknown";
}
