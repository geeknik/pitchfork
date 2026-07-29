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
#define M1_SYS_ERR_BAD_OP    (-1)   /* unknown operation code */
#define M1_SYS_ERR_BAD_ARG   (-2)   /* args NULL or otherwise invalid */

/* ---- System / info domain: m1_sys_call(op, args) ----
 * args is ignored for every op below; the requested value is the return. This
 * is the reference umbrella that establishes the pattern; new domains follow it.
 */
enum {
    M1_SYS_OP_API_LEVEL = 0,   /* kernel app-API level (M1_APP_API_VERSION) */
    M1_SYS_OP_HEAP_FREE = 1,   /* free system heap, in bytes */
    M1_SYS_OP_UPTIME_MS = 2,   /* milliseconds since boot */
};

#endif /* M1_APP_SYSCALL_H */
