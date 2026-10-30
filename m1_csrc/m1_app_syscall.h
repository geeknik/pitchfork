/* See COPYING.txt for license details. */

/*
 * m1_app_syscall.h — Grouped syscall dispatch for M1OS apps.
 *
 * To expose a wide operation surface without spending one flat export symbol per
 * operation (the symbol table is bounded), each domain is reached through a
 * single exported umbrella call:
 *
 *     int32_t m1_<domain>_call(uint32_t op, void *args);
 *
 * `op` selects the operation (an enum below); `args` points to a per-op argument
 * struct, or is NULL when the op needs none; the return is int32_t — a value, or
 * a negative M1_SYS_ERR_* on failure.
 *
 * ABI DISCIPLINE — this is what makes a flash-once runtime hold together:
 *   - Operation codes are ADDITIVE ONLY. Never renumber or remove one.
 *   - When you add an op, bump M1_APP_API_VERSION (m1_elf_loader.h) so an app
 *     that needs it can declare a minimum kernel API level in its .m1meta.
 *   - Old apps keep working across kernel updates; only apps that use a NEW op
 *     require a newer kernel. The loader already enforces the api_version gate.
 *
 * This header is mirrored into the app SDK (m1-sdk) so apps and kernel share one
 * definition of every op code.
 */

#ifndef M1_APP_SYSCALL_H
#define M1_APP_SYSCALL_H

#include <stdint.h>

/* Common negative return codes for umbrella calls. */
#define M1_SYS_ERR_BAD_OP    (-1)   /* unknown operation code (the NOT_SUPPORTED contract) */
#define M1_SYS_ERR_BAD_ARG   (-2)   /* args NULL or otherwise invalid */
#define M1_SYS_ERR_DENIED    (-3)   /* policy refusal (e.g. path outside the app sandbox) */
#define M1_SYS_ERR_IO        (-4)   /* filesystem/peripheral failure (detail in the response) */
#define M1_SYS_ERR_RANGE     (-5)   /* buffer too small / value out of range */

/* ---- System / info domain: m1_sys_call(op, args) ----
 * args is ignored for every op below; the requested value is the return. This
 * is the reference umbrella that establishes the pattern; new domains follow it.
 */
enum {
    M1_SYS_OP_API_LEVEL = 0,   /* kernel app-API level (M1_APP_API_VERSION) */
    M1_SYS_OP_HEAP_FREE = 1,   /* free system heap, in bytes */
    M1_SYS_OP_UPTIME_MS = 2,   /* milliseconds since boot */
    M1_SYS_OP_HEARTBEAT = 3,   /* supervised app: "still alive" (resets the
                                * hang grace; 1 when accepted, 0 if no app) */
    M1_SYS_OP_CANCEL_POLL = 4, /* 1 when the kernel asked this app to exit;
                                * a well-behaved loop checks this and returns
                                * from app_main promptly */
};

/* ========================================================================
 *  Storage domain: m1_storage_call(op, req, req_len, resp, resp_len)
 * ========================================================================
 * Replaces the raw FatFs exports (f_open/f_write/f_unlink/...) as the app-side
 * file API. Every request and response begins with the common header below, so
 * fields can be appended without breaking old apps: the kernel accepts any
 * req_len >= the minimum it needs for the op (unknown trailing bytes ignored)
 * and always writes hdr.size = the FULL current layout size, so an app can
 * detect a newer kernel's extra fields. Unknown op -> M1_SYS_ERR_BAD_OP.
 *
 * PATH SANDBOX — enforced before any filesystem call:
 *   - Relative paths resolve under "0:/AppData/" (created on demand).
 *   - Absolute paths must match a read-only library prefix ("0:/apps/",
 *     "0:/infrared/", "0:/System/") for read-class ops; WRITE/REMOVE/RENAME
 *     targets are confined to "0:/AppData/" (and must already live there).
 *   - "..", "\\", control characters, and >63-char components are rejected.
 * WRITE is atomic: temp file -> f_sync -> replace, never in-place.
 */

#define M1_STO_PATH_MAX    64u
#define M1_STO_IO_MAX      256u   /* max bytes per READ/WRITE/APPEND call; loop for more */
#define M1_STO_LIST_MAX    16u    /* max dirents per LIST call */

/* Common leading fields of every umbrella request/response (packed, LE).
 * Domain-agnostic: storage, ui, and every future domain starts with this. */
typedef struct __attribute__((packed)) {
    uint16_t size;      /* sizeof(the full struct) as built by the caller */
    uint8_t  version;   /* layout version of this op's structs; 1 */
    uint8_t  flags;     /* op-specific bits; unknown bits must be 0 */
} m1_call_hdr_t;

#define M1_CALL_LAYOUT_V1   1u
#define M1_STO_FLAG_TRUNC  0x01u  /* storage LIST response: total > count */  /* LIST response: total > count, call again */

/* --- Requests (one per op shape; the op code says which) --- */

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char path[M1_STO_PATH_MAX];   /* NUL-terminated */
} m1_sto_path_req_t;              /* STAT, REMOVE, MKDIR */

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char path[M1_STO_PATH_MAX];
    uint32_t offset;              /* READ: byte offset to start at */
    uint16_t length;              /* READ: bytes wanted, 1..M1_STO_IO_MAX */
} m1_sto_read_req_t;

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char path[M1_STO_PATH_MAX];
    uint16_t length;              /* payload bytes that follow in data[] */
    uint8_t  data[M1_STO_IO_MAX];
} m1_sto_write_req_t;             /* WRITE (atomic replace) and APPEND */

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char from[M1_STO_PATH_MAX];
    char to[M1_STO_PATH_MAX];
} m1_sto_rename_req_t;            /* both sides must be inside the sandbox */

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char path[M1_STO_PATH_MAX];
    uint16_t max_entries;         /* 1..M1_STO_LIST_MAX */
} m1_sto_list_req_t;

/* --- Responses --- */

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;             /* hdr.size always sizeof(full struct) */
    uint8_t  exists;
    uint8_t  is_dir;
    uint32_t file_size;
    uint32_t fresult;             /* raw FatFs code of the underlying op, 0 = OK */
} m1_sto_stat_resp_t;

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    uint16_t length;              /* bytes actually read (short = EOF) */
    uint32_t fresult;
    uint8_t  data[M1_STO_IO_MAX];
} m1_sto_read_resp_t;

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    uint32_t fresult;
} m1_sto_simple_resp_t;           /* WRITE, APPEND, MKDIR, REMOVE, RENAME */

typedef struct __attribute__((packed)) {
    char     name[40];            /* matches the stable m1app_dirent_t fields */
    uint32_t size;
    uint8_t  is_dir;
} m1_sto_dirent_t;

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    uint16_t count;               /* entries written below */
    uint16_t total;               /* entries in the directory (may exceed count) */
    uint32_t fresult;
    m1_sto_dirent_t entries[M1_STO_LIST_MAX];
} m1_sto_list_resp_t;

/* Shared request-header validation for every umbrella domain: minimum length
 * met, known layout version, no undefined request flags. Returns 0 or the
 * negative M1_SYS_ERR_* to return directly to the caller. */
static inline int m1_call_hdr_ok(const m1_call_hdr_t *hdr,
                                 uint16_t req_len, uint16_t min_len)
{
    if (hdr == 0 || req_len < min_len)
        return M1_SYS_ERR_BAD_ARG;
    if (hdr->version != M1_CALL_LAYOUT_V1)
        return M1_SYS_ERR_BAD_ARG;
    if (hdr->flags != 0)         /* no request-side flags defined yet */
        return M1_SYS_ERR_BAD_ARG;
    return 0;
}

enum {
    M1_STO_OP_STAT   = 0,
    M1_STO_OP_READ   = 1,
    M1_STO_OP_WRITE  = 2,   /* create-or-replace whole file from <=256B payloads */
    M1_STO_OP_APPEND = 3,   /* append payload at end; f_sync before close */
    M1_STO_OP_MKDIR  = 4,
    M1_STO_OP_REMOVE = 5,
    M1_STO_OP_RENAME = 6,
    M1_STO_OP_LIST   = 7,
};

/* Umbrella entry point (implemented in m1_app_storage.c; exported to apps
 * through the table in m1_app_api.c). resp may be NULL for ops that return
 * everything in the int32_t. Returns 0 or positive on success (op-specific:
 * STAT 0, READ = bytes read, LIST = entries written), negative M1_SYS_ERR_*. */
int32_t m1_storage_call(uint32_t op, const void *req, uint16_t req_len,
                        void *resp, uint16_t resp_len);

/* ========================================================================
 *  UI / display domain: m1_ui_call(op, req, req_len, resp, resp_len)
 * ========================================================================
 * The display is the first leased resource (m1_ui_focus.c): exactly one task
 * may draw at a time — the running foreground app, or the kernel's own UI
 * when the lease is free. The app manager grants the lease to the app task
 * at start and revokes it at exit.
 *
 * Direct drawing primitives (u8g2_*, m1app_display_flush) remain exported as
 * frozen hot paths for existing apps; the umbrella provides the focus-GATED
 * path (FLUSH refuses with M1_SYS_ERR_DENIED when another task owns the
 * lease) and the lease query. Enforcement of the legacy primitive waits for
 * the next API level so shipped apps keep working.
 */

enum {
    M1_UI_OP_FOCUS_QUERY = 0,   /* no req/resp: 1 if the CALLING task holds
                                 * the display lease, 0 if not */
    M1_UI_OP_FLUSH        = 1,  /* no req: flush the framebuffer; focus-gated */
    M1_UI_OP_NOTIFY       = 2,  /* modal message box for duration_ms; drawn in
                                 * the caller's context (caller must hold the
                                 * lease or the lease must be free) */
};

#define M1_UI_MSG_MAX       64u
#define M1_UI_NOTIFY_MAX_MS 10000u

typedef struct __attribute__((packed)) {
    m1_call_hdr_t hdr;
    char msg[M1_UI_MSG_MAX];    /* NUL-terminated */
    uint16_t duration_ms;       /* 1..M1_UI_NOTIFY_MAX_MS */
} m1_ui_notify_req_t;

/* Kernel-side hooks the umbrella calls once policy has approved (implemented
 * in m1_app_api.c where the display lives; host tests substitute their own). */
uint8_t m1_ui_hook_flush(void);
void m1_ui_hook_notify(const char *msg, uint16_t duration_ms);

int32_t m1_ui_call(uint32_t op, const void *req, uint16_t req_len,
                   void *resp, uint16_t resp_len);

#endif /* M1_APP_SYSCALL_H */
