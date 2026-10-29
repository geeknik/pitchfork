/* See COPYING.txt for license details. */

/*
 * m1_app_storage.c — storage-domain umbrella for M1OS apps.
 *
 * Implements m1_storage_call() (contract in m1_app_syscall.h): the sandboxed,
 * versioned, atomic file API that replaces handing apps raw FatFs entry
 * points. Everything here follows the umbrella discipline: ops are additive,
 * requests/responses start with size/version/flags, unknown ops return
 * M1_SYS_ERR_BAD_OP, and the path policy below runs BEFORE any f_* call.
 *
 * The path policy (m1_storage_resolve_path) is deliberately a pure function
 * with no FatFs dependency so it is host-testable (m1_csrc/test/).
 */

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ff.h"
#include "m1_app_syscall.h"

/*************************** D E F I N E S ************************************/

#define TAG "APPSTO"
/* Deliberately no m1_log_debug.h dependency: this module is host-testable
 * (m1_csrc/test/) and unknown ops are reported to the caller as
 * M1_SYS_ERR_BAD_OP, matching m1_sys_call's silent contract. */

#define M1_STO_SANDBOX_ROOT     "0:/AppData"
#define M1_STO_TMP_SUFFIX       ".part"

/* Absolute prefixes an app may READ from (case-insensitive, FatFs is too). */
static const char *const s_read_prefixes[] = {
    "0:/AppData/",
    "0:/apps/",
    "0:/infrared/",
    "0:/System/",
};
#define M1_STO_READ_PREFIX_COUNT (sizeof(s_read_prefixes) / sizeof(s_read_prefixes[0]))

/* Longest resolved path we can build: sandbox + '/' + 63-char relative. */
#define M1_STO_RESOLVED_MAX     96

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static bool sto_hdr_ok(const m1_sto_hdr_t *hdr, uint16_t req_len, uint16_t min_len);
static bool sto_path_copy(char *dst, uint32_t cap, const char *src);
static FRESULT sto_ensure_sandbox(void);
static FRESULT sto_atomic_write(const char *resolved, const uint8_t *data, uint16_t len);
static FRESULT sto_append(const char *resolved, const uint8_t *data, uint16_t len);

/*************************** C O D E ******************************************/

/*
 * Pure path policy — the security boundary of this umbrella. Resolves `in`
 * against the sandbox and checks the class rule. No filesystem access.
 * write_class 0 = read, 1 = write/remove/rename target. Returns 0, or
 * M1_SYS_ERR_* on refusal. `out` receives the resolved FatFs path on success.
 */
int m1_storage_resolve_path(uint8_t write_class, const char *in,
                            char *out, uint32_t out_cap)
{
    if (!in || !out || out_cap == 0)
        return M1_SYS_ERR_BAD_ARG;

    bool absolute = (in[0] == '0' && in[1] == ':' && in[2] == '/');
    const char *rel = absolute ? in + 3 : in;   /* skip "0:/" */
    if (*rel == '\0')
        return M1_SYS_ERR_BAD_ARG;              /* "0:/" alone: nothing to act on */

    /* Character screen: no backslashes, no control chars, sane length. */
    uint32_t rel_len = 0;
    for (const char *p = rel; *p; p++, rel_len++) {
        uint8_t c = (uint8_t)*p;
        if (c < 0x20 || c == 0x7F || c == '\\')
            return M1_SYS_ERR_DENIED;
        if (c == ':' && p != in)                /* drive colon only at "0:" */
            return M1_SYS_ERR_DENIED;
    }
    if (rel_len > M1_STO_PATH_MAX - 1)
        return M1_SYS_ERR_RANGE;
    /* One optional trailing '/' (directory style) is trimmed, not rejected. */
    if (rel[rel_len - 1] == '/' && rel_len > 1)
        rel_len--;

    /* Component screen: "." and ".." are refused; empty components ("a//b") too. */
    uint32_t seg_start = 0;
    for (uint32_t i = 0; i <= rel_len; i++) {
        if (i == rel_len || rel[i] == '/') {
            uint32_t seg_len = i - seg_start;
            if (seg_len == 0)
                return M1_SYS_ERR_BAD_ARG;
            if (seg_len == 1 && rel[seg_start] == '.')
                return M1_SYS_ERR_BAD_ARG;
            if (seg_len == 2 && rel[seg_start] == '.' && rel[seg_start + 1] == '.')
                return M1_SYS_ERR_DENIED;
            seg_start = i + 1;
        }
    }

    if (absolute) {
        /* Class rule on the raw absolute path, case-insensitively. */
        bool allowed = false;
        for (uint32_t i = 0; i < M1_STO_READ_PREFIX_COUNT; i++) {
            size_t plen = strlen(s_read_prefixes[i]);
            if (strlen(in) < plen)
                continue;
            if (strncasecmp(in, s_read_prefixes[i], plen) == 0) {
                allowed = (write_class == 0) ||
                          (i == 0);   /* writes only into the sandbox */
                break;
            }
        }
        if (!allowed)
            return M1_SYS_ERR_DENIED;
        if (strlen(in) >= out_cap)
            return M1_SYS_ERR_RANGE;
        strcpy(out, in);
        /* Re-trim the trailing slash the caller may have had. */
        size_t olen = strlen(out);
        if (olen > 3 && out[olen - 1] == '/')
            out[olen - 1] = '\0';
        return 0;
    }

    /* Relative: resolve under the sandbox root. */
    size_t root_len = strlen(M1_STO_SANDBOX_ROOT);
    if (root_len + 1 + rel_len + 1 > out_cap)
        return M1_SYS_ERR_RANGE;
    memcpy(out, M1_STO_SANDBOX_ROOT, root_len);
    out[root_len] = '/';
    memcpy(out + root_len + 1, rel, rel_len);
    out[root_len + 1 + rel_len] = '\0';
    return 0;
}

static bool sto_hdr_ok(const m1_sto_hdr_t *hdr, uint16_t req_len, uint16_t min_len)
{
    if (!hdr || req_len < min_len)
        return false;
    if (hdr->version != M1_STO_LAYOUT_V1)
        return false;
    if (hdr->flags & ~M1_STO_FLAG_TRUNC)        /* no flags defined on requests yet */
        return false;
    return true;
}

static bool sto_path_copy(char *dst, uint32_t cap, const char *src)
{
    size_t n = strnlen(src, cap);
    if (n == 0 || n == cap)                     /* empty or not NUL-terminated */
        return false;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static FRESULT sto_ensure_sandbox(void)
{
    FILINFO fi;
    FRESULT fr = f_stat(M1_STO_SANDBOX_ROOT, &fi);
    if (fr == FR_OK)
        return (fi.fattrib & AM_DIR) ? FR_OK : FR_DENIED;
    return f_mkdir(M1_STO_SANDBOX_ROOT);
}

/* Atomic one-shot write: temp -> sync -> replace, never in-place. Mirrors the
 * M1CP ".part" discipline. If the rename fails after the unlink the previous
 * file is gone — same residual window the RFID INI saver accepts; callers get
 * the FRESULT and can retry. */static FRESULT sto_atomic_write(const char *resolved, const uint8_t *data, uint16_t len)
{
    char tmp[M1_STO_RESOLVED_MAX + 8];
    if (snprintf(tmp, sizeof(tmp), "%s%s", resolved, M1_STO_TMP_SUFFIX) <= 0)
        return FR_INVALID_NAME;

    FIL f;
    FRESULT fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
        return fr;
    UINT written = 0;
    fr = f_write(&f, data, len, &written);
    if (fr == FR_OK && written != len)
        fr = FR_DENIED;
    if (fr == FR_OK)
        fr = f_sync(&f);
    f_close(&f);
    if (fr != FR_OK) {
        f_unlink(tmp);
        return fr;
    }

    f_unlink(resolved);                          /* ok if FR_NO_FILE */
    fr = f_rename(tmp, resolved);
    if (fr != FR_OK)
        f_unlink(tmp);
    return fr;
}

static FRESULT sto_append(const char *resolved, const uint8_t *data, uint16_t len)
{
    FIL f;
    FRESULT fr = f_open(&f, resolved, FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK)
        return fr;
    UINT written = 0;
    fr = f_write(&f, data, len, &written);
    if (fr == FR_OK && written != len)
        fr = FR_DENIED;
    if (fr == FR_OK)
        fr = f_sync(&f);                        /* log-style durability */
    f_close(&f);
    return fr;
}

/*
 * The umbrella. See m1_app_syscall.h for the op set and layout rules. resp
 * may be NULL where the op needs no response payload; otherwise resp_len is
 * validated against the FULL current layout and hdr.size reports that layout.
 */
int32_t m1_storage_call(uint32_t op, const void *req, uint16_t req_len,
                        void *resp, uint16_t resp_len)
{
    char resolved[M1_STO_RESOLVED_MAX];

    switch (op) {

    case M1_STO_OP_STAT: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_path_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_path_req_t *r = req;
        if (!resp || resp_len < sizeof(m1_sto_stat_resp_t))
            return M1_SYS_ERR_RANGE;
        m1_sto_stat_resp_t *o = resp;
        memset(o, 0, sizeof(*o));
        o->hdr.size = sizeof(*o);
        o->hdr.version = M1_STO_LAYOUT_V1;
        int pr = m1_storage_resolve_path(0, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        FILINFO fi;
        FRESULT fr = f_stat(resolved, &fi);
        o->fresult = (uint32_t)fr;
        if (fr == FR_OK) {
            o->exists = 1;
            o->is_dir = (fi.fattrib & AM_DIR) ? 1u : 0u;
            o->file_size = fi.fsize;
        } else if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            o->exists = 0;                       /* absent is an answer, not an error */
        } else {
            return M1_SYS_ERR_IO;
        }
        return 0;
    }

    case M1_STO_OP_READ: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_read_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_read_req_t *r = req;
        if (!resp || resp_len < sizeof(m1_sto_read_resp_t))
            return M1_SYS_ERR_RANGE;
        if (r->length == 0 || r->length > M1_STO_IO_MAX)
            return M1_SYS_ERR_RANGE;
        m1_sto_read_resp_t *o = resp;
        memset(o, 0, sizeof(*o));
        o->hdr.size = sizeof(*o);
        o->hdr.version = M1_STO_LAYOUT_V1;
        int pr = m1_storage_resolve_path(0, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        FIL f;
        FRESULT fr = f_open(&f, resolved, FA_READ);
        if (fr != FR_OK) {
            o->fresult = (uint32_t)fr;
            return M1_SYS_ERR_IO;
        }
        fr = f_lseek(&f, r->offset);
        if (fr == FR_OK) {
            UINT got = 0;
            fr = f_read(&f, o->data, r->length, &got);
            o->length = (uint16_t)got;
        }
        f_close(&f);
        o->fresult = (uint32_t)fr;
        return (fr == FR_OK) ? (int32_t)o->length : M1_SYS_ERR_IO;
    }

    case M1_STO_OP_WRITE:
    case M1_STO_OP_APPEND: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_write_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_write_req_t *r = req;
        if (r->length > M1_STO_IO_MAX)
            return M1_SYS_ERR_RANGE;
        int pr = m1_storage_resolve_path(1, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        FRESULT fr = sto_ensure_sandbox();
        if (fr != FR_OK)
            return M1_SYS_ERR_IO;
        fr = (op == M1_STO_OP_WRITE)
                 ? sto_atomic_write(resolved, r->data, r->length)
                 : sto_append(resolved, r->data, r->length);
        if (resp) {
            if (resp_len < sizeof(m1_sto_simple_resp_t))
                return M1_SYS_ERR_RANGE;
            m1_sto_simple_resp_t *o = resp;
            memset(o, 0, sizeof(*o));
            o->hdr.size = sizeof(*o);
            o->hdr.version = M1_STO_LAYOUT_V1;
            o->fresult = (uint32_t)fr;
        }
        return (fr == FR_OK) ? 0 : M1_SYS_ERR_IO;
    }

    case M1_STO_OP_MKDIR: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_path_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_path_req_t *r = req;
        int pr = m1_storage_resolve_path(1, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        FRESULT fr = sto_ensure_sandbox();       /* MKDIR may be the first op ever */
        if (fr != FR_OK)
            return M1_SYS_ERR_IO;
        FILINFO fi;
        fr = f_stat(resolved, &fi);              /* idempotent: exists-as-dir = OK */
        if (fr == FR_OK)
            return (fi.fattrib & AM_DIR) ? 0 : M1_SYS_ERR_IO;
        fr = f_mkdir(resolved);
        return (fr == FR_OK) ? 0 : M1_SYS_ERR_IO;
    }

    case M1_STO_OP_REMOVE: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_path_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_path_req_t *r = req;
        int pr = m1_storage_resolve_path(1, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        FRESULT fr = f_unlink(resolved);
        return (fr == FR_OK) ? 0 : M1_SYS_ERR_IO;
    }

    case M1_STO_OP_RENAME: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_rename_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_rename_req_t *r = req;
        char resolved_to[M1_STO_RESOLVED_MAX];
        int pr = m1_storage_resolve_path(1, r->from, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        pr = m1_storage_resolve_path(1, r->to, resolved_to, sizeof(resolved_to));
        if (pr != 0)
            return pr;
        FRESULT fr = f_rename(resolved, resolved_to);
        return (fr == FR_OK) ? 0 : M1_SYS_ERR_IO;
    }

    case M1_STO_OP_LIST: {
        if (!sto_hdr_ok(req, req_len, sizeof(m1_sto_list_req_t)))
            return M1_SYS_ERR_BAD_ARG;
        const m1_sto_list_req_t *r = req;
        if (!resp || resp_len < sizeof(m1_sto_list_resp_t))
            return M1_SYS_ERR_RANGE;
        if (r->max_entries == 0 || r->max_entries > M1_STO_LIST_MAX)
            return M1_SYS_ERR_RANGE;
        m1_sto_list_resp_t *o = resp;
        memset(o, 0, sizeof(*o));
        o->hdr.size = sizeof(*o);
        o->hdr.version = M1_STO_LAYOUT_V1;
        int pr = m1_storage_resolve_path(0, r->path, resolved, sizeof(resolved));
        if (pr != 0)
            return pr;
        DIR d;
        FRESULT fr = f_opendir(&d, resolved);
        if (fr != FR_OK)
            return M1_SYS_ERR_IO;
        FILINFO fi;
        while ((fr = f_readdir(&d, &fi)) == FR_OK && fi.fname[0] != '\0') {
            if (strcmp(fi.fname, ".") == 0 || strcmp(fi.fname, "..") == 0)
                continue;
            o->total++;
            if (o->count < r->max_entries) {
                m1_sto_dirent_t *e = &o->entries[o->count++];
                sto_path_copy(e->name, sizeof(e->name), fi.fname);
                e->size = fi.fsize;
                e->is_dir = (fi.fattrib & AM_DIR) ? 1u : 0u;
            }
        }
        f_closedir(&d);
        if (fr != FR_OK)
            return M1_SYS_ERR_IO;
        if (o->total > o->count)
            o->hdr.flags |= M1_STO_FLAG_TRUNC;
        return (int32_t)o->count;
    }

    default:
        return M1_SYS_ERR_BAD_OP;
    }
}
